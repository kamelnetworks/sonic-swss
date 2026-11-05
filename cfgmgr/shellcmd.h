#ifndef __SHELLCMD__
#define __SHELLCMD__

#include <iomanip>
#include <regex>
#include <string>

#include "exec.h"

#define IP_CMD               "/sbin/ip"
#define BRIDGE_CMD           "/sbin/bridge"
#define BRCTL_CMD            "/sbin/brctl"
#define ECHO_CMD             "/bin/echo"
#define BASH_CMD             "/bin/bash"
#define GREP_CMD             "/bin/grep"
#define TEAMD_CMD            "/usr/bin/teamd"
#define TEAMDCTL_CMD         "/usr/bin/teamdctl"
#define IPTABLES_CMD         "/sbin/iptables"
#define CONNTRACK_CMD        "/usr/sbin/conntrack"

/* Name of the Dot1Q bridge the VLAN member ports are enslaved to */
#define DOT1Q_BRIDGE_NAME    "Bridge"

#define EXEC_WITH_ERROR_THROW(cmd, res)   ({    \
    int ret = swss::exec(cmd, res);             \
    if (ret != 0)                               \
    {                                           \
        throw runtime_error(cmd + " : " + res); \
    }                                           \
})

static inline std::string shellquote(const std::string& str)
{
    static const std::regex re("([$`\"\\\n])");
    return "\"" + std::regex_replace(str, re, "\\$1") + "\"";
}

/*
 * Look up the kernel master (the Dot1Q bridge or a port channel) of a netdev.
 * Returns false if the netdev does not exist. On success, master is the name
 * of the master device, or empty if the netdev has none.
 */
static inline bool getNetdevMaster(const std::string& dev, std::string& master)
{
    std::string res;
    std::string cmd = IP_CMD " -o link show dev " + shellquote(dev);

    master.clear();
    if (swss::exec(cmd, res) != 0)
    {
        return false;
    }

    static const std::regex re("\\bmaster (\\S+)");
    std::smatch m;
    if (std::regex_search(res, m, re))
    {
        master = m[1];
    }
    return true;
}

#endif /* __SHELLCMD__ */
