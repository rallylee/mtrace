#include <map>
#include <vector>
#include <set>
#include <stack>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include "percallstack.hh"
#include "bininfo.hh"
#include <elf++.hh>
#include <dwarf++.hh>

class AbstractSharing : public EntryHandler {
public:
    AbstractSharing(bool ascopes, bool unexpected)
        : ascopes_(ascopes), unexpected_(unexpected) {
        int fd = open("mscan.kern", O_RDONLY);
        if (fd < 0)
            die("failed to open o.qemu/kernel.elf");
        elf_ = elf::elf(elf::create_mmap_loader(fd));
        dwarf_ = dwarf::dwarf(dwarf::elf::create_loader(elf_));
    }

    virtual void handle(const union mtrace_entry* entry) {
        if (entry->h.type == mtrace_entry_fcall) {
            // This needs to happen whether we're active or not
            callstacks_.handle(&entry->fcall, this);
            return;
        }

        if (!guest_enabled_mtrace())
            return;
        if (mtrace_enable.access.mode != mtrace_record_ascope)
            die("Abstract sharing analysis requires mtrace_record_ascope mode");

        int cpu = entry->h.cpu;

        if (entry->h.type == mtrace_entry_ascope) {
            if (callstacks_.current(cpu))
	      callstacks_.current(cpu)->handle(&entry->ascope);
        } else if (entry->h.type == mtrace_entry_avar) {
            if (callstacks_.current(cpu))
	      callstacks_.current(cpu)->handle(&entry->avar);
        } else if (entry->h.type == mtrace_entry_access) {
            if (callstacks_.current(cpu))
                callstacks_.current(cpu)->handle(&entry->access);
        }
    }

    virtual void exit(JsonDict *json_file) {
        callstacks_.flush();

        JsonList *ascopes = nullptr;
        JsonList *unexpected = nullptr;

        int compared_scopes = 0, shared_scopes[2][2] = {};

        if (ascopes_) {
            // Raw abstract and concrete sets
            ascopes = JsonList::create();
            JsonList *lst = ascopes;
            for (auto &ascope : scopes_) {
                JsonDict *od = JsonDict::create();
                od->put("name", ascope.name_);
                od->put("aread", JsonList::create(ascope.aread_.begin(), ascope.aread_.end()));
                od->put("awrite", JsonList::create(ascope.awrite_.begin(), ascope.awrite_.end()));

                JsonList *rw;
                rw = JsonList::create();
                for (auto &it : ascope.read_)
                    rw->append(it.second.to_json(dwarf_));
                od->put("read", rw);
                rw = JsonList::create();
                for (auto &it : ascope.write_)
                    rw->append(it.second.to_json(dwarf_));
                od->put("write", rw);

                lst->append(od);
            }
        }

        if (unexpected_) {
            // Processed sets
            // XXX Would be nice to order these by the amount of sharing
            // XXX Produce a summary of sharing so its more obvious
            // when you screw up
            unexpected = JsonList::create();
            JsonList *lst = unexpected;
            for (auto it1 = scopes_.begin(); it1 != scopes_.end(); ++it1) {
                const Ascope &s1 = *it1;
                for (auto it2 = it1+1; it2 != scopes_.end(); ++it2) {
                    const Ascope &s2 = *it2;
                    // If the two scopes ran on the same CPU, we'll
                    // get lots of "sharing" on per-CPU data, so don't
                    // compare scopes from the same CPU
                    if (s1.cpu_ == s2.cpu_)
                        continue;

                    compared_scopes++;

                    auto abstract_sharing =
                        shares(s1.aread_.begin(),  s1.aread_.end(),
                               s1.awrite_.begin(), s1.awrite_.end(),

                               s2.aread_.begin(),  s2.aread_.end(),
                               s2.awrite_.begin(), s2.awrite_.end());
                    auto concrete_sharing =
                        shares(s1.read_.begin(),  s1.read_.end(),
                               s1.write_.begin(), s1.write_.end(),

                               s2.read_.begin(),  s2.read_.end(),
                               s2.write_.begin(), s2.write_.end());

                    shared_scopes[!!abstract_sharing][!!concrete_sharing]++;

                    if (concrete_sharing && !abstract_sharing) {
                        JsonDict *od = JsonDict::create();
                        od->put("s1", s1.name_);
                        od->put("s2", s2.name_);
                        JsonList *shared = JsonList::create();
                        shared_to_json(shared,
                                       s1.read_.begin(),  s1.read_.end(),
                                       s2.write_.begin(), s2.write_.end());
                        shared_to_json(shared,
                                       s1.write_.begin(), s1.write_.end(),
                                       s2.read_.begin(),  s2.read_.end());
                        shared_to_json(shared,
                                       s1.write_.begin(), s1.write_.end(),
                                       s2.write_.begin(), s2.write_.end());
                        od->put("shared", shared);
                        lst->append(od);
                    } else if (abstract_sharing && !concrete_sharing) {
                        fprintf(stderr, "Warning: Abstract sharing without concrete sharing: %s and %s (%s)\n",
                                s1.name_.c_str(), s2.name_.c_str(), abstract_sharing->c_str());
                    }
                }
            }
        }

        // Summary
        JsonDict *summary = JsonDict::create();
        summary->put("total scopes", (uint64_t) scopes_.size());
        if (unexpected_) {
            summary->put("compared scopes", compared_scopes);
            // In order of badness
            summary->put("logically unshared/physically unshared", shared_scopes[0][0]);
            summary->put("logically shared  /physically shared",   shared_scopes[1][1]);
            summary->put("logically unshared/physically shared",   shared_scopes[0][1]);
            if (shared_scopes[1][0])
                summary->put("logically shared  /physically unshared (imprecise spec)",
                             shared_scopes[1][0]);
        }

        json_file->put("scope-summary", summary);
        if (ascopes)
            json_file->put("abstract-scopes", ascopes);
        if (unexpected)
            json_file->put("unexpected-sharing", unexpected);
    }

    struct PhysicalAccess {
        string type;
        uint64_t base;
        uint64_t access;
        uint64_t pc;

        JsonDict *to_json(const dwarf::dwarf &dw, const PhysicalAccess *other = nullptr) const
        {
            JsonDict *out = JsonDict::create();
            if (type.size()) {
                out->put("addr", resolve_type_offset(dw, type, base, access - base, pc));
            } else {
                char buf[64];
                sprintf(buf, "0x%"PRIx64, access);
                out->put("addr", buf);
            }
            if (other && pc != other->pc) {
                out->put("pc1", addr2line->function_description(pc));
                out->put("pc2", addr2line->function_description(other->pc));
            } else {
                out->put("pc", addr2line->function_description(pc));
            }
            return out;
        }

        bool operator<(const PhysicalAccess &o) const
        {
            return access < o.access;
        }
    };

    class Ascope {
    public:
        Ascope(string name, int cpu)
            : name_(name), cpu_(cpu) { }

        string name_;
        int cpu_;
        set<string> aread_;
        set<string> awrite_;
        map<uint64_t, PhysicalAccess> read_;
        map<uint64_t, PhysicalAccess> write_;
    };

private:
    elf::elf elf_;
    dwarf::dwarf dwarf_;

    bool ascopes_, unexpected_;

    class CallStack
    {
        AbstractSharing *a_;
        vector<Ascope> stack_;

        void pop()
        {
                // XXX Lots of copying
                Ascope *cur = &stack_.back();
                if (!cur->aread_.empty() || !cur->awrite_.empty())
                    a_->scopes_.push_back(*cur);
                stack_.pop_back();
        }

    public:
        CallStack(const mtrace_fcall_entry *fcall, AbstractSharing *a)
            : a_(a) {}
        ~CallStack()
        {
            while (!stack_.empty())
                pop();
        }

        void handle(const mtrace_ascope_entry *ascope)
        {
            if (ascope->exit)
                pop();
            else
                stack_.push_back(Ascope(ascope->name, ascope->h.cpu));
        }

        void handle(const mtrace_avar_entry *avar)
        {
	    if (stack_.empty()) {
	        fprintf(stderr, "avar without ascope\n");
		return;
	    }

            Ascope *cur = &stack_.back();
            string var = avar->name;
            if (avar->write) {
                cur->awrite_.insert(var);
                cur->aread_.erase(var);
            } else {
                if (!cur->awrite_.count(var))
                    cur->aread_.insert(var);
            }
        }

        void handle(const mtrace_access_entry *access)
        {
            if (stack_.empty())
                return;

            // Since QEMU limits the granularity of tracking to 4
            // bytes in ascope mode, we need to do that, too.
            auto addr = access->guest_addr & ~3;
            MtraceObject obj;
            PhysicalAccess pa;
            if (mtrace_label_map.object(addr, obj)) {
                pa.type = obj.name_;
                pa.base = obj.guest_addr_;
            } else {
                pa.base = 0;
            }
            pa.access = access->guest_addr;
            pa.pc = access->pc;

            // Physical accesses apply to all scopes on the stack.
            // This is necessary to make sure that each logical scope
            // completely captures the physical accesses done by it
            // and on its behalf.  (Note that interrupts get
            // completely separate callstacks, so this does *not*
            // bleed across asynchronous event boundaries.)
            for (auto &scope : stack_) {
                switch (access->access_type) {
                case mtrace_access_st:
                case mtrace_access_iw:
                    if (!scope.write_.count(addr))
                        scope.write_[addr] = pa;
                    scope.read_.erase(addr);
                    break;
                case mtrace_access_ld:
                    if (!scope.write_.count(addr))
                        scope.read_[addr] = pa;
                    break;
                default:
                    die("AbstractSharing::CallStack::handle: unknown access type");
                }
            }
        }
    };

    PerCallStack<CallStack> callstacks_;

    vector<Ascope> scopes_;

    template<class InputIterator1, class InputIterator2>
    static decltype(&(**((InputIterator1*)0)))
        shares(InputIterator1 r1begin, InputIterator1 r1end,
               InputIterator1 w1begin, InputIterator1 w1end,
               InputIterator2 r2begin, InputIterator2 r2end,
               InputIterator2 w2begin, InputIterator2 w2end)
    {
        return
            intersects(r1begin, r1end, w2begin, w2end) ?:
            intersects(w1begin, w1end, r2begin, r2end) ?:
            intersects(w1begin, w1end, w2begin, w2end);
    }

    template<class InputIterator1, class InputIterator2>
    static decltype(&(**((InputIterator1*)0)))
        intersects(InputIterator1 first1, InputIterator1 last1,
                   InputIterator2 first2, InputIterator2 last2)
    {
        while (first1 != last1 && first2 != last2) {
            if (*first1 < *first2)
                ++first1;
            else if (*first2 < *first1)
                ++first2;
            else
                return &(*first1);
        }
        return nullptr;
    }

    template<class InputIterator1, class InputIterator2>
    void shared_to_json(JsonList *shared,
                        InputIterator1 first1, InputIterator1 last1,
                        InputIterator2 first2, InputIterator2 last2)
    {
        while (first1 != last1 && first2 != last2) {
            if (*first1 < *first2)
                ++first1;
            else if (*first2 < *first1)
                ++first2;
            else {
                shared->append(first1->second.to_json(dwarf_, &first2->second));
                first1++;
                first2++;
            }
        }
    }
};