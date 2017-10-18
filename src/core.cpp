#include "core.hpp"
#include "config.hpp"
#include "property.hpp"
#include "util/unix.hpp"
#include "util/log.hpp"

extern "C" {
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
}

TError TCore::Register(const TPath &portod) {
    std::string limit, pattern;
    TError error;

    if (!config().core().enable())
        return TError::Success();

    error = GetSysctl("kernel.core_pipe_limit", limit);
    if (error || limit == "0") {
        /* wait for herlper exit - keep pidns alive */
        error = SetSysctl("kernel.core_pipe_limit", "1024");
        if (error)
            return error;
    }

    pattern = "|" + portod.ToString() + " core %P %I %p %i %s %d %c " +
              StringReplaceAll(config().core().default_pattern(), " ", "__SPACE__");
    return SetSysctl("kernel.core_pattern", pattern);
}

TError TCore::Unregister() {
    if (!config().core().enable())
        return TError::Success();

    return SetSysctl("kernel.core_pattern", config().core().default_pattern());
}

TError TCore::Handle(const TTuple &args) {

    if (args.size() < 7)
        return TError(EError::Unknown, "should be executed via sysctl kernel.core_pattern");

    Pid = std::stoi(args[0]);
    Tid = std::stoi(args[1]);
    Vpid = std::stoi(args[2]);
    Vtid = std::stoi(args[3]);
    Signal = std::stoi(args[4]);
    Dumpable = std::stoi(args[5]);
    Ulimit = std::stoull(args[6]);
    if (args.size() > 7)
        DefaultPattern = StringReplaceAll(args[7], "__SPACE__", " ");

    ProcessName = GetTaskName(Pid);
    ThreadName = GetTaskName(Tid);

    if (!Ulimit || !Dumpable)
        return TError::Success();

    OpenLog(PORTO_LOG);
    SetProcessName("portod-core");

    TError error;
    error = Identify();

    if (!error && CoreCommand.size()) {
        L_ACT("Forward core from container {}: {} {} signal {}",
                Container, ProcessName, Pid, Signal);
        error = Forward();
        if (error)
            L("Cannot forward core from container {}: {}", Container, error);
    }

    if ((error || !CoreCommand.size()) && DefaultPattern) {
        L_ACT("Save core from container {}: {} pid {} signal {}",
                Container, ProcessName, Pid, Signal);

        error = Save();
        if (error)
            L("Cannot save core from container {}: {}", Container, error);
    }

    return error;
}

TError TCore::Identify() {
    std::map<std::string, std::string> cgmap;
    TError error;

    error = GetTaskCgroups(Pid, cgmap);
    if (error || cgmap.find("freezer") == cgmap.end())
        return TError(EError::Unknown, "freezer not found");

    auto cg = cgmap["freezer"];
    if (!StringStartsWith(cg, std::string(PORTO_CGROUP_PREFIX) + "/"))
        return TError(EError::InvalidState, "non-porto freezer");

    Container = cg.substr(strlen(PORTO_CGROUP_PREFIX) + 1);

    //FIXME ugly
    if (Conn.GetProperty(Container, P_CORE_COMMAND, CoreCommand) ||
            Conn.GetProperty(Container, P_USER, User) ||
            Conn.GetProperty(Container, P_GROUP, Group) ||
            Conn.GetProperty(Container, P_OWNER_USER, OwnerUser) ||
            Conn.GetProperty(Container, P_OWNER_GROUP, OwnerGroup) ||
            Conn.GetProperty(Container, P_CWD, Cwd))
        return TError(EError::Unknown, "cannot dump container");

    Slot = Container.substr(0, Container.find('/'));

    if (UserId(OwnerUser, OwnerUid))
        OwnerUid = -1;

    if (GroupId(OwnerGroup, OwnerGid))
        OwnerGid = -1;

    return TError::Success();
}

TError TCore::Forward() {
    std::string core = Container + "/core-" + std::to_string(Pid);
    TMultiTuple env = {
        {"CORE_PID", std::to_string(Vpid)},
        {"CORE_TID", std::to_string(Vtid)},
        {"CORE_SIG", std::to_string(Signal)},
        {"CORE_TASK_NAME", GetTaskName(Pid)},
        {"CORE_THREAD_NAME", GetTaskName(Tid)},
        {"CORE_CONTAINER", Container},
    };

    if (Conn.CreateWeakContainer(core) ||
            Conn.SetProperty(core, P_ISOLATE, "false") ||
            Conn.SetProperty(core, P_STDIN_PATH, "/dev/fd/0") ||
            Conn.SetProperty(core, P_STDOUT_PATH, "/dev/null") ||
            Conn.SetProperty(core, P_STDERR_PATH, "/dev/null") ||
            Conn.SetProperty(core, P_COMMAND, CoreCommand) ||
            Conn.SetProperty(core, P_USER, User) ||
            Conn.SetProperty(core, P_GROUP, Group) ||
            Conn.SetProperty(core, P_OWNER_USER, OwnerUser) ||
            Conn.SetProperty(core, P_OWNER_GROUP, OwnerGroup) ||
            Conn.SetProperty(core, P_CWD, Cwd) ||
            Conn.SetProperty(core, P_ENV, MergeEscapeStrings(env, '=', ';')) ||
            Conn.Start(core))
        return TError(EError::Unknown, "cannot create core container");

    L("Forwading core into {}", core);

    std::string result;
    Conn.WaitContainers({core}, result, config().core().timeout_s());
    Conn.Destroy(core);
    return TError::Success();
}

TError TCore::Save() {
    TPath dir = DefaultPattern.DirName();
    std::vector<std::string> names;
    TError error;

    error = dir.ReadDirectory(names);
    if (error)
        return error;

    uint64_t TotalSize = 0;
    uint64_t SlotSize = 0;

    for (auto &name: names) {
        struct stat st;

        if (!(dir / name).StatStrict(st)) {
            TotalSize += st.st_blocks * 512 / st.st_nlink;

            auto sep = name.find('%');
            if (sep != std::string::npos && name.substr(0, sep) == Slot)
                SlotSize += st.st_blocks * 512;
        }

        if ((TotalSize >> 20) >= config().core().space_limit_mb())
            return TError(EError::ResourceNotAvailable,
                    fmt::format("Total core size reached limit: {}M of {}M",
                        TotalSize >> 20, config().core().space_limit_mb()));

        if ((SlotSize >> 20) >= config().core().slot_space_limit_mb())
            return TError(EError::ResourceNotAvailable,
                    fmt::format("Slot {} core size reached limit: {}M of {}M",
                        Slot, SlotSize >> 20, config().core().slot_space_limit_mb()));
    }

    std::string filter = "";
    std::string format = ".core";

    if (StringEndsWith(DefaultPattern.ToString(), ".gz")) {
        filter = "gzip";
        format = ".core.gz";
    }

    if (StringEndsWith(DefaultPattern.ToString(), ".xz")) {
        filter = "xz";
        format = ".core.xz";
    }

    auto prefix = Container.size() ? StringReplaceAll(Container, "/", "%") + "%" : "";
    Pattern = dir / ( prefix + ProcessName + "." + std::to_string(Pid) +
              ".S" + std::to_string(Signal) + "." +
              FormatTime(time(nullptr), "%Y%m%dT%H%M%S") + format);

    TFile file;
    error = file.Create(Pattern, O_RDWR | O_CREAT | O_EXCL, 0440);
    if (error)
        return error;

    error = file.Chown(OwnerUid, OwnerGid);
    if (error)
        L("Cannot chown core: {}", error);

    error = DefaultPattern.Hardlink(Pattern);
    if (error) {
        L("Cannot hardlink core to default pattern: {}", error);
        DefaultPattern = "";
    }

    L("Dumping core into {} ({})", Pattern, DefaultPattern.BaseName());

    off_t size = 0;
    off_t data = 0;

    if (filter.size()) {
        if (file.Fd != STDOUT_FILENO &&
                dup2(file.Fd, STDOUT_FILENO) != STDOUT_FILENO)
            return TError(EError::Unknown, errno, "dup2");
        execlp(filter.c_str(), filter.c_str(), nullptr);
        error = TError(EError::Unknown, errno, "cannot execute filter " + filter);
    } else {
        uint64_t buf[512];
        off_t sync_start = 0;
        off_t sync_block = 1 << 20;

        error = TError::Success();
        do {
            size_t len = 0;

            do {
                ssize_t ret = read(STDIN_FILENO, (uint8_t*)buf + len, sizeof(buf) - len);
                if (ret <= 0) {
                    if (ret < 0)
                        error = TError(EError::Unknown, errno, "read");
                    break;
                }
                len += ret;
            } while (len < sizeof(buf));

            bool zero = true;
            for (size_t i = 0; zero && i < sizeof(buf) / sizeof(buf[0]); i++)
                zero = !buf[i];

            if (zero && len == sizeof(buf)) {
                size += len;
                continue;
            }

            size_t off = 0;
            while (off < len) {
                ssize_t ret = pwrite(file.Fd, (uint8_t*)buf + off, len - off, size + off);
                if (ret <= 0) {
                    if (ret < 0)
                        error = TError(EError::Unknown, errno, "write");
                    break;
                }
                off += ret;
            }
            size += off;
            data += off;

            if (size > sync_start + sync_block * 2) {
                (void)sync_file_range(file.Fd, sync_start, size - sync_start,
                        SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE);
                sync_start = size - sync_block;
            }

            if (off < len || !len)
                break;

        } while (1);

        if (ftruncate(file.Fd, size))
            L("Cannot truncate core dump");
        fdatasync(file.Fd);
    }

    if (!error) {
        L("Core dump {} ({}) written: {} data, {} holes, {} total",
                Pattern, DefaultPattern.BaseName(), StringFormatSize(data),
                StringFormatSize(size - data), StringFormatSize(size));
    } else if (!data) {
        Pattern.Unlink();
        if (DefaultPattern)
            DefaultPattern.Unlink();
    }

    return error;
}
