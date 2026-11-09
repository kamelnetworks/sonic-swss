#include "gtest/gtest.h"
#include "../mock_table.h"
#include "macaddress.h"
#include "shellcmd.h"
#define private public
#define protected public
#include "vlanmgr.h"
#undef protected
#undef private

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;
extern swss::MacAddress gMacAddress;

namespace vlanmgr_ut
{
    using namespace swss;
    using namespace std;

    static const string VLAN = "Vlan3999";
    static const string PORT = "Ethernet64";
    static const string CFG_KEY = VLAN + "|" + PORT;
    static const string APP_KEY = VLAN + ":" + PORT;

    /* Kernel master reported by "ip -o link show dev <port>" */
    static string mock_port_master;
    /* Exit status of the "bridge vlan del ..." command */
    static int mock_bridge_vlan_del_rc;
    /* Nonzero: "ip -o link show dev <port>" fails with this status and prints mock_link_show_err */
    static int mock_link_show_rc;
    static string mock_link_show_err;

    static const string LINK_SHOW_NO_DEVICE = "Device \"" + PORT + "\" does not exist.\n";
    static const string LINK_SHOW_FAILED = "Cannot send link get request: No buffer space available\n";

    static int cb(const string &cmd, string &stdout)
    {
        mockCallArgs.push_back(cmd);
        if (cmd.find("/sbin/ip -o link show dev") != string::npos)
        {
            if (mock_link_show_rc != 0)
            {
                stdout = mock_link_show_err;
                return mock_link_show_rc;
            }
            stdout = "47: " + PORT + ": <BROADCAST,MULTICAST> mtu 9100 qdisc noop ";
            if (!mock_port_master.empty())
            {
                stdout += "master " + mock_port_master + " ";
            }
            stdout += "state DOWN mode DEFAULT group default qlen 1000\\    link/ether 00:11:22:33:44:55 brd ff:ff:ff:ff:ff:ff\n";
            return 0;
        }
        if (cmd.find("/sbin/bridge vlan del vid 3999") != string::npos)
        {
            stdout = "RTNETLINK answers: Operation not supported";
            return mock_bridge_vlan_del_rc;
        }
        return 0;
    }

    static bool commandSeen(const string &needle)
    {
        for (const auto &c : mockCallArgs)
        {
            if (c.find(needle) != string::npos)
            {
                return true;
            }
        }
        return false;
    }

    struct VlanMgrTest : public ::testing::Test
    {
        shared_ptr<DBConnector> m_config_db;
        shared_ptr<DBConnector> m_app_db;
        shared_ptr<DBConnector> m_state_db;
        shared_ptr<VlanMgr> m_vlanMgr;
        shared_ptr<Table> m_cfgVlanMemberTable;
        shared_ptr<Table> m_appVlanMemberTable;
        shared_ptr<Table> m_stateVlanMemberTable;
        Consumer *m_memberConsumer;

        virtual void SetUp() override
        {
            testing_db::reset();
            m_config_db = make_shared<DBConnector>("CONFIG_DB", 0);
            m_app_db = make_shared<DBConnector>("APPL_DB", 0);
            m_state_db = make_shared<DBConnector>("STATE_DB", 0);

            m_cfgVlanMemberTable = make_shared<Table>(m_config_db.get(), CFG_VLAN_MEMBER_TABLE_NAME);
            m_appVlanMemberTable = make_shared<Table>(m_app_db.get(), APP_VLAN_MEMBER_TABLE_NAME);
            m_stateVlanMemberTable = make_shared<Table>(m_state_db.get(), STATE_VLAN_MEMBER_TABLE_NAME);

            gMacAddress = MacAddress("00:11:22:33:44:55");
            mockCallArgs.clear();
            mock_port_master.clear();
            mock_bridge_vlan_del_rc = 0;
            mock_link_show_rc = 0;
            mock_link_show_err.clear();
            callback = cb;

            /* Same table lists as vlanmgrd.cpp */
            vector<string> cfg_vlan_tables = {
                CFG_VLAN_TABLE_NAME,
                CFG_VLAN_MEMBER_TABLE_NAME,
            };
            vector<string> state_vlan_tables = {
                STATE_OPER_PORT_TABLE_NAME,
                STATE_OPER_FDB_TABLE_NAME,
                STATE_OPER_VLAN_MEMBER_TABLE_NAME
            };
            m_vlanMgr = make_shared<VlanMgr>(m_config_db.get(), m_app_db.get(), m_state_db.get(),
                                             cfg_vlan_tables, state_vlan_tables);
            m_memberConsumer = dynamic_cast<Consumer *>(m_vlanMgr->getExecutor(CFG_VLAN_MEMBER_TABLE_NAME));
            ASSERT_NE(m_memberConsumer, nullptr);
            /* Forget the bridge setup commands the constructor ran */
            mockCallArgs.clear();

            /* The VLAN and the port are ready */
            Table state_vlan_table(m_state_db.get(), STATE_VLAN_TABLE_NAME);
            state_vlan_table.set(VLAN, { { "state", "ok" } });
            Table state_port_table(m_state_db.get(), STATE_PORT_TABLE_NAME);
            state_port_table.set(PORT, { { "state", "ok" } });
        }

        virtual void TearDown() override
        {
            callback = nullptr;
        }

        void configureMember()
        {
            m_cfgVlanMemberTable->set(CFG_KEY, { { "tagging_mode", "untagged" } });
            m_vlanMgr->addExistingData(m_cfgVlanMemberTable.get());
        }

        bool memberPublished()
        {
            vector<FieldValueTuple> fvs;
            return m_appVlanMemberTable->get(APP_KEY, fvs);
        }
    };

    /* A port enslaved by a port channel is not pulled into the bridge; the
     * VLAN member waits (with the entry kept for retry) until the port leaves
     * the port channel. */
    TEST_F(VlanMgrTest, MemberDeferredWhilePortIsLagMember)
    {
        mock_port_master = "PortChannel99";
        configureMember();
        m_vlanMgr->doTask();

        EXPECT_FALSE(commandSeen("master Bridge"));
        EXPECT_FALSE(memberPublished());
        EXPECT_EQ(m_memberConsumer->m_toSync.size(), 1);
        EXPECT_EQ(m_vlanMgr->m_deferredVlanMembers.count(CFG_KEY), 1);

        /* Still in the port channel: still deferred */
        mockCallArgs.clear();
        m_vlanMgr->doTask();
        EXPECT_FALSE(commandSeen("master Bridge"));
        EXPECT_EQ(m_memberConsumer->m_toSync.size(), 1);

        /* teammgrd released the port: the retry adds it to the bridge */
        mock_port_master = "";
        mockCallArgs.clear();
        m_vlanMgr->doTask();
        EXPECT_TRUE(commandSeen("master Bridge && /sbin/bridge vlan del vid 1"));
        EXPECT_TRUE(memberPublished());
        EXPECT_EQ(m_memberConsumer->m_toSync.size(), 0);
        EXPECT_TRUE(m_vlanMgr->m_deferredVlanMembers.empty());
        vector<FieldValueTuple> fvs;
        EXPECT_TRUE(m_stateVlanMemberTable->get(CFG_KEY, fvs));
    }

    /* The same deferral when the port channel membership is only configured
     * (CONFIG_DB PORTCHANNEL_MEMBER) and teammgrd has not enslaved the port yet */
    TEST_F(VlanMgrTest, MemberDeferredWhileLagMemberConfigured)
    {
        Table cfg_lag_member_table(m_config_db.get(), CFG_LAG_MEMBER_TABLE_NAME);
        cfg_lag_member_table.set("PortChannel99|" + PORT, { { "NULL", "NULL" } });
        /* Another port's membership doesn't count */
        cfg_lag_member_table.set("PortChannel99|Ethernet4", { { "NULL", "NULL" } });
        mock_port_master = "";
        configureMember();
        m_vlanMgr->doTask();

        EXPECT_FALSE(commandSeen("master Bridge"));
        EXPECT_FALSE(memberPublished());
        EXPECT_EQ(m_memberConsumer->m_toSync.size(), 1);
        EXPECT_EQ(m_vlanMgr->m_deferredVlanMembers.count(CFG_KEY), 1);

        /* The port channel membership is removed from the config: the retry adds the port */
        cfg_lag_member_table.del("PortChannel99|" + PORT);
        mockCallArgs.clear();
        m_vlanMgr->doTask();
        EXPECT_TRUE(commandSeen("master Bridge && /sbin/bridge vlan del vid 1"));
        EXPECT_TRUE(memberPublished());
        EXPECT_EQ(m_memberConsumer->m_toSync.size(), 0);
        EXPECT_TRUE(m_vlanMgr->m_deferredVlanMembers.empty());
    }

    /* A port that is already in the bridge, or has no master, is added as before */
    TEST_F(VlanMgrTest, MemberAddedWhenPortIsNotLagMember)
    {
        mock_port_master = "Bridge";
        configureMember();
        m_vlanMgr->doTask();
        EXPECT_TRUE(commandSeen("master Bridge"));
        EXPECT_TRUE(memberPublished());
        EXPECT_EQ(m_memberConsumer->m_toSync.size(), 0);
    }

    /* Removing a deferred member from the config forgets it */
    TEST_F(VlanMgrTest, DeferredMemberRemovedFromConfig)
    {
        mock_port_master = "PortChannel99";
        configureMember();
        m_vlanMgr->doTask();
        EXPECT_EQ(m_vlanMgr->m_deferredVlanMembers.count(CFG_KEY), 1);

        m_memberConsumer->addToSync(KeyOpFieldsValuesTuple{ CFG_KEY, DEL_COMMAND, {} });
        mockCallArgs.clear();
        m_vlanMgr->doTask();
        EXPECT_FALSE(commandSeen("bridge vlan del"));
        EXPECT_EQ(m_memberConsumer->m_toSync.size(), 0);
        EXPECT_TRUE(m_vlanMgr->m_deferredVlanMembers.empty());
    }

    /* Deleting a member whose port was taken over by a port channel: the
     * kernel "bridge vlan del" fails, but the removal finishes instead of
     * being retried forever, and the port is not detached from the team. */
    TEST_F(VlanMgrTest, MemberDeleteFinishesWhenPortLeftBridge)
    {
        /* The member is applied, then the port gets enslaved by a port channel */
        configureMember();
        m_vlanMgr->doTask();
        ASSERT_TRUE(memberPublished());
        ASSERT_EQ(m_memberConsumer->m_toSync.size(), 0);

        mock_port_master = "PortChannel99";
        mock_bridge_vlan_del_rc = 1;
        m_memberConsumer->addToSync(KeyOpFieldsValuesTuple{ CFG_KEY, DEL_COMMAND, {} });
        mockCallArgs.clear();
        m_vlanMgr->doTask();

        EXPECT_TRUE(commandSeen("bridge vlan del vid 3999"));
        EXPECT_TRUE(commandSeen("/sbin/ip -o link show dev"));
        EXPECT_EQ(m_memberConsumer->m_toSync.size(), 0);
        EXPECT_FALSE(memberPublished());
        vector<FieldValueTuple> fvs;
        EXPECT_FALSE(m_stateVlanMemberTable->get(CFG_KEY, fvs));
        EXPECT_TRUE(m_vlanMgr->m_PortVlanMember[PORT].empty());
    }

    /* A failing "bridge vlan del" on a port that is still in the bridge is a
     * real error and is still retried */
    TEST_F(VlanMgrTest, MemberDeleteRetriedWhenPortStillInBridge)
    {
        configureMember();
        m_vlanMgr->doTask();
        ASSERT_TRUE(memberPublished());

        mock_port_master = "Bridge";
        mock_bridge_vlan_del_rc = 1;
        m_memberConsumer->addToSync(KeyOpFieldsValuesTuple{ CFG_KEY, DEL_COMMAND, {} });
        m_vlanMgr->doTask();

        EXPECT_EQ(m_memberConsumer->m_toSync.size(), 1);
        EXPECT_TRUE(memberPublished());
    }

    /* Review of kamelnetworks/sonic-swss#2: a failed master lookup is not proof
     * that the port left the bridge. When both "bridge vlan del" and the lookup
     * fail, the removal is retried and the published membership is kept; the
     * retry then completes it. */
    TEST_F(VlanMgrTest, MemberDeleteRetriedWhenMasterLookupFails)
    {
        configureMember();
        m_vlanMgr->doTask();
        ASSERT_TRUE(memberPublished());

        mock_bridge_vlan_del_rc = 1;
        mock_link_show_rc = 1;
        mock_link_show_err = LINK_SHOW_FAILED;
        m_memberConsumer->addToSync(KeyOpFieldsValuesTuple{ CFG_KEY, DEL_COMMAND, {} });
        mockCallArgs.clear();
        m_vlanMgr->doTask();

        EXPECT_TRUE(commandSeen("bridge vlan del vid 3999"));
        EXPECT_TRUE(commandSeen("/sbin/ip -o link show dev"));
        EXPECT_EQ(m_memberConsumer->m_toSync.size(), 1);
        EXPECT_TRUE(memberPublished());
        vector<FieldValueTuple> fvs;
        EXPECT_TRUE(m_stateVlanMemberTable->get(CFG_KEY, fvs));
        EXPECT_FALSE(m_vlanMgr->m_PortVlanMember[PORT].empty());

        /* The kernel recovers: the retry removes the member */
        mock_bridge_vlan_del_rc = 0;
        mock_link_show_rc = 0;
        mockCallArgs.clear();
        m_vlanMgr->doTask();
        EXPECT_TRUE(commandSeen("bridge vlan del vid 3999"));
        EXPECT_EQ(m_memberConsumer->m_toSync.size(), 0);
        EXPECT_FALSE(memberPublished());
        EXPECT_FALSE(m_stateVlanMemberTable->get(CFG_KEY, fvs));
        EXPECT_TRUE(m_vlanMgr->m_PortVlanMember[PORT].empty());
    }

    /* A port that no longer exists has no kernel VLAN membership left: the
     * failed "bridge vlan del" finishes the removal instead of looping */
    TEST_F(VlanMgrTest, MemberDeleteFinishesWhenPortIsGone)
    {
        configureMember();
        m_vlanMgr->doTask();
        ASSERT_TRUE(memberPublished());

        mock_bridge_vlan_del_rc = 1;
        mock_link_show_rc = 1;
        mock_link_show_err = LINK_SHOW_NO_DEVICE;
        m_memberConsumer->addToSync(KeyOpFieldsValuesTuple{ CFG_KEY, DEL_COMMAND, {} });
        m_vlanMgr->doTask();

        EXPECT_EQ(m_memberConsumer->m_toSync.size(), 0);
        EXPECT_FALSE(memberPublished());
    }

    /* The add side: with no PORTCHANNEL_MEMBER row left, a failed master lookup
     * must not let vlanmgrd pull a port that may still be in a port channel
     * into the bridge. The member is deferred and added once the lookup works. */
    TEST_F(VlanMgrTest, MemberDeferredWhenMasterLookupFails)
    {
        mock_link_show_rc = 1;
        mock_link_show_err = LINK_SHOW_FAILED;
        configureMember();
        m_vlanMgr->doTask();

        EXPECT_TRUE(commandSeen("/sbin/ip -o link show dev"));
        EXPECT_FALSE(commandSeen("master Bridge"));
        EXPECT_FALSE(memberPublished());
        EXPECT_EQ(m_memberConsumer->m_toSync.size(), 1);
        EXPECT_EQ(m_vlanMgr->m_deferredVlanMembers.count(CFG_KEY), 1);

        /* Still unknown: still deferred */
        mockCallArgs.clear();
        m_vlanMgr->doTask();
        EXPECT_FALSE(commandSeen("master Bridge"));
        EXPECT_EQ(m_memberConsumer->m_toSync.size(), 1);

        /* The lookup works and the port has no master: it is added */
        mock_link_show_rc = 0;
        mockCallArgs.clear();
        m_vlanMgr->doTask();
        EXPECT_TRUE(commandSeen("master Bridge"));
        EXPECT_TRUE(memberPublished());
        EXPECT_EQ(m_memberConsumer->m_toSync.size(), 0);
        EXPECT_TRUE(m_vlanMgr->m_deferredVlanMembers.empty());
    }
}
