#include "gtest/gtest.h"
#include "../mock_table.h"
#include "macaddress.h"
#include "shellcmd.h"
#define private public
#define protected public
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

            /* Same table list as vxlanmgrd.cpp */
            vector<string> tables = {
                CFG_VNET_TABLE_NAME,
                CFG_VXLAN_TUNNEL_TABLE_NAME,
                CFG_VXLAN_TUNNEL_MAP_TABLE_NAME,
                CFG_VXLAN_EVPN_NVO_TABLE_NAME,
                CFG_VLAN_TABLE_NAME,
            };
            m_vxlanMgr = make_shared<VxlanMgr>(m_config_db.get(), m_app_db.get(), m_state_db.get(), tables);

            /* vxlanmgrd main(): reconcile window opens at start and closes on the first select timeout */
            m_vxlanMgr->beginReconcile(false);
            m_vxlanMgr->endReconcile(false);
            mockCallArgs.clear();
        }

        /*
         * Configure VLAN <vlan> (with the given CONFIG_DB fields, state ok), the source VTEP
         * and a <vlan> <-> <vni> tunnel map, then let vxlanmgrd process it.
         */
        void createMapping(const string &vlan, const string &vni, const vector<FieldValueTuple> &vlanFvs)
        {
            m_cfgVlanTable->set(vlan, vlanFvs);
            m_stateVlanTable->set(vlan, { {"state", "ok"} });
            m_cfgTunnelTable->set(TUNNEL, { {"src_ip", SRC_IP} });
            m_cfgTunnelMapTable->set(TUNNEL + "|map_" + vni + "_" + vlan, { {"vlan", vlan}, {"vni", vni} });

            m_vxlanMgr->addExistingData(m_cfgVlanTable.get());
            m_vxlanMgr->addExistingData(m_cfgTunnelTable.get());
            m_vxlanMgr->addExistingData(m_cfgTunnelMapTable.get());
            m_vxlanMgr->doTask();
        }

        /* Change the VLAN's CONFIG_DB fields and let vxlanmgrd process it. */
        void updateVlan(const string &vlan, const vector<FieldValueTuple> &vlanFvs)
        {
            m_cfgVlanTable->set(vlan, vlanFvs);
            m_vxlanMgr->addExistingData(m_cfgVlanTable.get());
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
    };

    /* kamelnetworks/sonic-buildimage#74: the VXLAN netdev must carry the VLAN's MTU, not the kernel default 1500. */
    TEST_F(VxlanMgrTest, NetdevCreatedWithVlanMtu)
    {
        createMapping("Vlan100", "100", { {"vlanid", "100"}, {"mtu", "9000"} });

        string cmd = linkAddCmd("vtep-100");
        ASSERT_NE("", cmd) << "vtep-100 was not created";
        EXPECT_NE(string::npos, cmd.find(" mtu 9000 type vxlan ")) << cmd;
        /* Unrelated parts of the command are unchanged */
        EXPECT_NE(string::npos, cmd.find(" address 00:11:22:33:44:55 ")) << cmd;
        EXPECT_NE(string::npos, cmd.find(" type vxlan id 100 local " + SRC_IP + " nolearning ")) << cmd;
        EXPECT_NE(string::npos, cmd.find(" dstport 4789")) << cmd;
    }

    /* A VLAN without an explicit mtu gets the SONiC default (9100), like vlanmgrd gives the Bridge. */
    TEST_F(VxlanMgrTest, NetdevCreatedWithDefaultMtuWhenVlanHasNone)
    {
        createMapping("Vlan200", "200", { {"vlanid", "200"} });

        string cmd = linkAddCmd("vtep-200");
        ASSERT_NE("", cmd) << "vtep-200 was not created";
        EXPECT_NE(string::npos, cmd.find(" mtu 9100 type vxlan ")) << cmd;
    }

    /* An mtu that is not a number is not interpolated into the shell command; the default is used. */
    TEST_F(VxlanMgrTest, NetdevCreatedWithDefaultMtuWhenVlanMtuIsInvalid)
    {
        createMapping("Vlan300", "300", { {"vlanid", "300"}, {"mtu", "9000; touch /tmp/x"} });

        string cmd = linkAddCmd("vtep-300");
        ASSERT_NE("", cmd) << "vtep-300 was not created";
        EXPECT_NE(string::npos, cmd.find(" mtu 9100 type vxlan ")) << cmd;
        EXPECT_EQ(string::npos, cmd.find("touch")) << cmd;
    }

    /* Changing the VLAN MTU is applied to the mapped netdev; VLANs without a map are ignored. */
    TEST_F(VxlanMgrTest, VlanMtuChangeUpdatesNetdev)
    {
        createMapping("Vlan100", "100", { {"vlanid", "100"}, {"mtu", "9000"} });
        ASSERT_NE("", linkAddCmd("vtep-100"));
        mockCallArgs.clear();

        updateVlan("Vlan100", { {"vlanid", "100"}, {"mtu", "1600"} });
        EXPECT_EQ(1u, countCmds(string(IP_CMD) + " link set dev vtep-100 mtu 1600"))
            << "commands: " << ::testing::PrintToString(mockCallArgs);

        /* The update was consumed: a second doTask() does nothing more */
        mockCallArgs.clear();
        m_vxlanMgr->doTask();
        EXPECT_TRUE(mockCallArgs.empty()) << ::testing::PrintToString(mockCallArgs);

        /* An unmapped VLAN produces no netdev commands */
        updateVlan("Vlan300", { {"vlanid", "300"}, {"mtu", "1500"} });
        EXPECT_TRUE(mockCallArgs.empty()) << ::testing::PrintToString(mockCallArgs);
    }

    /* Deleting the map removes the netdev, and a later VLAN MTU change no longer touches it. */
    TEST_F(VxlanMgrTest, VlanMtuChangeAfterMapDeleteIsIgnored)
    {
        createMapping("Vlan100", "100", { {"vlanid", "100"}, {"mtu", "9000"} });
        ASSERT_NE("", linkAddCmd("vtep-100"));

        m_cfgTunnelMapTable->del(TUNNEL + "|map_100_Vlan100");
        auto consumer = dynamic_cast<Consumer *>(m_vxlanMgr->getExecutor(CFG_VXLAN_TUNNEL_MAP_TABLE_NAME));
        ASSERT_NE(nullptr, consumer);
        consumer->addToSync({ TUNNEL + "|map_100_Vlan100", DEL_COMMAND, {} });
        mockCallArgs.clear();
        m_vxlanMgr->doTask();
        EXPECT_EQ(1u, countCmds(string(IP_CMD) + " link del dev vtep-100"))
            << ::testing::PrintToString(mockCallArgs);

        mockCallArgs.clear();
        updateVlan("Vlan100", { {"vlanid", "100"}, {"mtu", "1600"} });
        EXPECT_TRUE(mockCallArgs.empty()) << ::testing::PrintToString(mockCallArgs);
    }
}
