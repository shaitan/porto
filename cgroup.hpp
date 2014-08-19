#ifndef __CGROUP_HPP__
#define __CGROUP_HPP__

#include <string>
#include <unordered_map>

#include "error.hpp"
#include "subsystem.hpp"
#include "util/mount.hpp"
#include "util/folder.hpp"

const std::string RootCgroup = "porto";

class TCgroup {
    const std::string name;
    const std::shared_ptr<TCgroup> parent;
    int level;
    std::vector<std::weak_ptr<TCgroup>> children;

    std::shared_ptr<TMount> mount;
    std::vector<std::shared_ptr<TSubsystem>> subsystems;

    std::string tmpfs = "/sys/fs/cgroup";
    mode_t mode = 0x666;

    bool need_cleanup = false;

    bool RemoveSubtree(void);

public:
    static std::shared_ptr<TCgroup> Get(const std::string &name,
                                        const std::shared_ptr<TCgroup> &parent);
    static std::shared_ptr<TCgroup> GetRoot(const std::shared_ptr<TMount> mount, const std::vector<std::shared_ptr<TSubsystem>> subsystems);
    static std::shared_ptr<TCgroup> GetRoot(const std::shared_ptr<TSubsystem> subsystem);

    TCgroup(const std::string &name, const std::shared_ptr<TCgroup> parent) :
        name(name), parent(parent), level(parent->level + 1) { }
    TCgroup(const std::shared_ptr<TMount> mount, const std::vector<std::shared_ptr<TSubsystem>> subsystems) :
        name("/"), parent(std::shared_ptr<TCgroup>(nullptr)), level(0), mount(mount), subsystems(subsystems) { }

    TCgroup(const std::vector<std::shared_ptr<TSubsystem>> controller);

    ~TCgroup();

    void SetNeedCleanup() {
        need_cleanup = true;
    }

    bool IsRoot() const;

    std::string Path();
    std::string Relpath();

    TError Create();
    TError Remove();

    TError Kill(int signal);

    std::vector<std::shared_ptr<TCgroup> > FindChildren();

    TError GetProcesses(std::vector<pid_t> &processes);
    TError GetTasks(std::vector<pid_t> &tasks);
    bool IsEmpty();

    TError Attach(int pid);

    TError GetKnobValue(const std::string &knob, std::string &value);
    TError GetKnobValueAsLines(const std::string &knob, std::vector<std::string> &lines);
    TError SetKnobValue(const std::string &knob, const std::string &value, bool append = false);
    bool HasSubsystem(const std::string &name);

    friend bool operator==(const TCgroup& c1, const TCgroup& c2);
    friend std::ostream& operator<<(std::ostream& os, const TCgroup& cg);
};

class TCgroupSnapshot {
    std::vector<std::shared_ptr<TCgroup> > cgroups;
    std::unordered_map<std::string, std::shared_ptr<TSubsystem>> subsystems; // can be net_cls _or_ net_prio
public:
    TError Create();
    friend std::ostream& operator<<(std::ostream& os, const TCgroupSnapshot& st);
};

#endif
