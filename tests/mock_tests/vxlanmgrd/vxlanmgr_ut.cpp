#include "gtest/gtest.h"
#include "../mock_table.h"
#include "macaddress.h"
#include "shellcmd.h"
#define private public
#define protected public
#include "warm_restart.h"
#include "vxlanmgr.h"
#undef protected
#undef private

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;
extern swss::MacAddress gMacAddress;

namespace vxlanmgr_ut
{
    using namespace swss;
    using namespace std;

    static const string TUNNEL = "vtep";
    static const string SRC_IP = "10.0.0.1";
    static const string SRC_IP6 = "fc00::1";

    /*
     * Shell mock for the failure and warm-restart cases: records every command like the
     * default mock, fails the next <g_failMtuSets> "ip link set dev ... mtu" commands, and
     * answers "ip link show type vxlan" with <g_vxlanLinks>.
     */
    static int g_failMtuSets = 0;
    static string g_vxlanLinks;

    static int recordingExec(const string &cmd, string &stdout)
    {
        mockCallArgs.push_back(cmd);
        stdout = "";
        if (cmd.find(" link show type vxlan") != string::npos)
        {
            stdout = g_vxlanLinks;
        }
        else if (cmd.find(" link set dev ") != string::npos && cmd.find(" mtu ") != string::npos &&
                 g_failMtuSets > 0)
        {
            g_failMtuSets--;
            return 1;
        }
        return 0;
    }

    /* Same table list as vxlanmgrd.cpp */
    static const vector<string> VXLANMGRD_TABLES = {
        CFG_VNET_TABLE_NAME,
        CFG_VXLAN_TUNNEL_TABLE_NAME,
        CFG_VXLAN_TUNNEL_MAP_TABLE_NAME,
        CFG_VXLAN_EVPN_NVO_TABLE_NAME,
    };

    static const string NETDEV_LINK_1500 =
        "86: vtep-100: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc noqueue master Bridge state UNKNOWN mode DEFAULT group default qlen 1000\n";

    struct VxlanMgrTest : public ::testing::Test
    {
        shared_ptr<DBConnector> m_config_db;
        shared_ptr<DBConnector> m_app_db;
        shared_ptr<DBConnector> m_state_db;
        shared_ptr<VxlanMgr> m_vxlanMgr;
        shared_ptr<Table> m_cfgVlanTable;
        shared_ptr<Table> m_cfgTunnelTable;
        shared_ptr<Table> m_cfgTunnelMapTable;
        shared_ptr<Table> m_stateVlanTable;

        virtual void SetUp() override
        {
            testing_db::reset();
            m_config_db = make_shared<DBConnector>("CONFIG_DB", 0);
            m_app_db = make_shared<DBConnector>("APPL_DB", 0);
            m_state_db = make_shared<DBConnector>("STATE_DB", 0);

            m_cfgVlanTable = make_shared<Table>(m_config_db.get(), CFG_VLAN_TABLE_NAME);
            m_cfgTunnelTable = make_shared<Table>(m_config_db.get(), CFG_VXLAN_TUNNEL_TABLE_NAME);
            m_cfgTunnelMapTable = make_shared<Table>(m_config_db.get(), CFG_VXLAN_TUNNEL_MAP_TABLE_NAME);
            m_stateVlanTable = make_shared<Table>(m_state_db.get(), STATE_VLAN_TABLE_NAME);

            gMacAddress = MacAddress("00:11:22:33:44:55");
            callback = nullptr;
            g_failMtuSets = 0;
            g_vxlanLinks = "";

            m_vxlanMgr = make_shared<VxlanMgr>(m_config_db.get(), m_app_db.get(), m_state_db.get(), VXLANMGRD_TABLES);

            /* vxlanmgrd main(): reconcile window opens at start and closes on the first select timeout */
            m_vxlanMgr->beginReconcile(false);
            m_vxlanMgr->endReconcile(false);
            mockCallArgs.clear();
        }

        virtual void TearDown() override
        {
            callback = nullptr;
            WarmStart::getInstance().m_enabled = false;
        }

        /*
         * Restart vxlanmgrd with <links> as the output of "ip link show type vxlan", i.e. with
         * VXLAN netdevs left over from before the restart, and stay in the reconcile window.
         */
        void restartWithExistingNetdevs(const string &links)
        {
            g_vxlanLinks = links;
            callback = recordingExec;
            /* Otherwise the constructor deletes the netdevs as stale */
            WarmStart::initialize("vxlanmgrd", "swss");
            WarmStart::getInstance().m_enabled = true;
            m_vxlanMgr = make_shared<VxlanMgr>(m_config_db.get(), m_app_db.get(), m_state_db.get(), VXLANMGRD_TABLES);
            m_vxlanMgr->beginReconcile(true);
            mockCallArgs.clear();
        }

        /* Delete a CONFIG_DB entry in <table> and let vxlanmgrd process the notification. */
        void delConfig(const string &table, const string &key)
        {
            Table t(m_config_db.get(), table);
            t.del(key);
            auto consumer = dynamic_cast<Consumer *>(m_vxlanMgr->getExecutor(table));
            ASSERT_NE(nullptr, consumer) << "vxlanmgrd does not subscribe to " << table;
            consumer->addToSync({ key, DEL_COMMAND, {} });
            m_vxlanMgr->doTask();
        }

        /*
         * The lab underlay: eBGP over PortChannel2 (mtu <mtu>) with an address in the default VRF,
         * and a router port Ethernet0 (mtu 1500). vxlanmgrd doesn't watch these tables.
         */
        void setUnderlay(const string &mtu)
        {
            Table(m_config_db.get(), CFG_LAG_TABLE_NAME).set("PortChannel2", { {"admin_status", "up"}, {"mtu", mtu} });
            Table(m_config_db.get(), CFG_LAG_INTF_TABLE_NAME).set("PortChannel2", {});
            Table(m_config_db.get(), CFG_LAG_INTF_TABLE_NAME).set("PortChannel2|10.1.0.0/31", {});
            Table(m_config_db.get(), CFG_PORT_TABLE_NAME).set("Ethernet0", { {"mtu", "1500"} });
            Table(m_config_db.get(), CFG_INTF_TABLE_NAME).set("Ethernet0|10.2.0.0/31", {});
        }

        /*
         * Configure VLAN <vlan> (with the given CONFIG_DB fields, state ok), the source VTEP
         * <srcIp> and a <vlan> <-> <vni> tunnel map, then let vxlanmgrd process it.
         */
        void createMapping(const string &vlan, const string &vni, const vector<FieldValueTuple> &vlanFvs,
                           const string &srcIp = SRC_IP)
        {
            m_cfgVlanTable->set(vlan, vlanFvs);
            m_stateVlanTable->set(vlan, { {"state", "ok"} });
            m_cfgTunnelTable->set(TUNNEL, { {"src_ip", srcIp} });
            m_cfgTunnelMapTable->set(TUNNEL + "|map_" + vni + "_" + vlan, { {"vlan", vlan}, {"vni", vni} });

            m_vxlanMgr->addExistingData(m_cfgTunnelTable.get());
            m_vxlanMgr->addExistingData(m_cfgTunnelMapTable.get());
            m_vxlanMgr->doTask();
        }

        /* The "ip link add <dev> ..." command issued for <dev>, or "" if none was. */
        static string linkAddCmd(const string &dev)
        {
            const string needle = string(IP_CMD) + " link add " + dev + " ";
            for (const auto &cmd : mockCallArgs)
            {
                if (cmd.find(needle) != string::npos)
                {
                    return cmd;
                }
            }
            return "";
        }

        /* The MTU vtep-100 was created with, or "" if it was not created */
        static string createdMtu(const string &dev = "vtep-100")
        {
            const string cmd = linkAddCmd(dev);
            const string needle = " mtu ";
            size_t pos = cmd.find(needle);
            if (pos == string::npos)
            {
                return "";
            }
            pos += needle.size();
            return cmd.substr(pos, cmd.find(' ', pos) - pos);
        }

        /* The MTU vxlanmgrd has recorded as applied to the netdev mapped to <vlan> */
        string cachedMtu(const string &vlan)
        {
            for (const auto &map : m_vxlanMgr->m_vxlanTunnelMapCache)
            {
                if (map.second.vlan == vlan)
                {
                    return map.second.mtu;
                }
            }
            return "<no map>";
        }

        static size_t countCmds(const string &needle)
        {
            size_t n = 0;
            for (const auto &cmd : mockCallArgs)
            {
                if (cmd.find(needle) != string::npos)
                {
                    n++;
                }
            }
            return n;
        }

        static string setMtuCmd(const string &mtu)
        {
            return string(IP_CMD) + " link set dev vtep-100 mtu " + mtu;
        }
    };

    /*
     * kamelnetworks/sonic-buildimage#74: the VXLAN netdev must not keep the kernel default 1500.
     * It gets the Bridge MTU 9100, the largest MTU a VlanN SVI can have, so it never limits SVI
     * traffic below the SVI's own MTU.
     */
    TEST_F(VxlanMgrTest, NetdevCreatedWithBridgeMtu)
    {
        createMapping("Vlan100", "100", { {"vlanid", "100"} });

        string cmd = linkAddCmd("vtep-100");
        ASSERT_NE("", cmd) << "vtep-100 was not created";
        EXPECT_NE(string::npos, cmd.find(" mtu 9100 type vxlan ")) << cmd;
        EXPECT_EQ("9100", cachedMtu("Vlan100"));
        /* Unrelated parts of the command are unchanged */
        EXPECT_NE(string::npos, cmd.find(" address 00:11:22:33:44:55 ")) << cmd;
        EXPECT_NE(string::npos, cmd.find(" type vxlan id 100 local " + SRC_IP + " nolearning ")) << cmd;
        EXPECT_NE(string::npos, cmd.find(" dstport 4789")) << cmd;

        /* Applied once: later passes issue nothing */
        mockCallArgs.clear();
        m_vxlanMgr->doTask();
        EXPECT_TRUE(mockCallArgs.empty()) << ::testing::PrintToString(mockCallArgs);
    }

    /*
     * Like Arista EOS, nothing is sized from the underlay: a 9000 PortChannel and a 1500 router
     * port in the default VRF leave the netdev at 9100, and vxlanmgrd doesn't watch the underlay.
     */
    TEST_F(VxlanMgrTest, NetdevMtuIgnoresUnderlay)
    {
        setUnderlay("9000");
        createMapping("Vlan100", "100", { {"vlanid", "100"} });
        EXPECT_EQ("9100", createdMtu()) << linkAddCmd("vtep-100");

        for (const auto &table : { CFG_PORT_TABLE_NAME, CFG_LAG_TABLE_NAME, CFG_INTF_TABLE_NAME,
                                   CFG_LAG_INTF_TABLE_NAME, CFG_VLAN_SUB_INTF_TABLE_NAME })
        {
            EXPECT_EQ(nullptr, m_vxlanMgr->getExecutor(table)) << table;
        }
    }

    /* The outer header size doesn't matter either: an IPv6 source VTEP gets the same MTU. */
    TEST_F(VxlanMgrTest, Ipv6SourceVtepGetsBridgeMtu)
    {
        createMapping("Vlan100", "100", { {"vlanid", "100"} }, SRC_IP6);

        ASSERT_NE("", linkAddCmd("vtep-100")) << "vtep-100 was not created";
        EXPECT_EQ("9100", createdMtu()) << linkAddCmd("vtep-100");
    }

    /*
     * The VLAN MTU doesn't size the netdev: the SVI (kernel VlanN, set by vlanmgrd) enforces it,
     * like the SVI MTU on EOS. vxlanmgrd doesn't watch VLAN.
     */
    TEST_F(VxlanMgrTest, VlanMtuDoesNotSizeNetdev)
    {
        createMapping("Vlan100", "100", { {"vlanid", "100"}, {"mtu", "1500"} });
        EXPECT_EQ("9100", createdMtu()) << linkAddCmd("vtep-100");
        EXPECT_EQ(nullptr, m_vxlanMgr->getExecutor(CFG_VLAN_TABLE_NAME));
    }

    /*
     * Warm restart: a netdev left by the previous run (possibly an image without this fix, so at
     * MTU 1500) is reconciled, not re-created, and gets the netdev MTU.
     */
    TEST_F(VxlanMgrTest, WarmReconciledNetdevGetsBridgeMtu)
    {
        restartWithExistingNetdevs(NETDEV_LINK_1500);
        ASSERT_EQ(1u, m_vxlanMgr->m_vxlanNetDevices.count("vtep-100"));

        createMapping("Vlan100", "100", { {"vlanid", "100"} });
        EXPECT_EQ("", linkAddCmd("vtep-100")) << "reconciled netdev was re-created";
        EXPECT_EQ(1u, countCmds(setMtuCmd("9100"))) << ::testing::PrintToString(mockCallArgs);
        EXPECT_EQ("9100", cachedMtu("Vlan100"));

        /* Ending the reconcile window keeps the reconciled netdev, and nothing is pending */
        mockCallArgs.clear();
        m_vxlanMgr->endReconcile(true);
        m_vxlanMgr->doTask();
        EXPECT_EQ(0u, countCmds("vtep-100")) << ::testing::PrintToString(mockCallArgs);
    }

    /*
     * Warm restart where setting the MTU on the reconciled netdev fails: it is not recorded as
     * applied, and it is retried on every doTask() pass (vxlanmgrd runs one on every select
     * timeout) until it succeeds.
     */
    TEST_F(VxlanMgrTest, WarmReconciledNetdevMtuFailureIsRetried)
    {
        restartWithExistingNetdevs(NETDEV_LINK_1500);

        /* The reconcile attempt and its first retry fail */
        g_failMtuSets = 2;
        createMapping("Vlan100", "100", { {"vlanid", "100"} });
        EXPECT_EQ("", linkAddCmd("vtep-100")) << "reconciled netdev was re-created";
        EXPECT_EQ("", cachedMtu("Vlan100"));

        mockCallArgs.clear();
        m_vxlanMgr->endReconcile(true);
        m_vxlanMgr->doTask();
        EXPECT_EQ(1u, countCmds(setMtuCmd("9100"))) << "MTU was not retried: " << ::testing::PrintToString(mockCallArgs);
        EXPECT_EQ("", cachedMtu("Vlan100"));

        mockCallArgs.clear();
        m_vxlanMgr->doTask();
        EXPECT_EQ(1u, countCmds(setMtuCmd("9100"))) << ::testing::PrintToString(mockCallArgs);
        EXPECT_EQ("9100", cachedMtu("Vlan100"));

        /* Once applied nothing is pending */
        mockCallArgs.clear();
        m_vxlanMgr->doTask();
        EXPECT_TRUE(mockCallArgs.empty()) << ::testing::PrintToString(mockCallArgs);
    }

    /* A pending MTU retry stops when the map (and with it the netdev) is deleted. */
    TEST_F(VxlanMgrTest, PendingMtuRetryStopsWhenMapIsDeleted)
    {
        restartWithExistingNetdevs(NETDEV_LINK_1500);

        /* Every attempt fails */
        g_failMtuSets = 100;
        createMapping("Vlan100", "100", { {"vlanid", "100"} });
        ASSERT_EQ("", cachedMtu("Vlan100"));
        m_vxlanMgr->endReconcile(true);

        mockCallArgs.clear();
        delConfig(CFG_VXLAN_TUNNEL_MAP_TABLE_NAME, TUNNEL + "|map_100_Vlan100");
        EXPECT_EQ(1u, countCmds(string(IP_CMD) + " link del dev vtep-100"))
            << ::testing::PrintToString(mockCallArgs);

        mockCallArgs.clear();
        m_vxlanMgr->doTask();
        m_vxlanMgr->doTask();
        EXPECT_EQ(0u, countCmds(" mtu ")) << ::testing::PrintToString(mockCallArgs);
    }
}
