#include "gtest/gtest.h"
#include "../mock_table.h"
#include "macaddress.h"
#define private public
#define protected public
#include "vlanmgr.h"
#undef protected
#undef private

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;
extern swss::MacAddress gMacAddress;

namespace vlanmgr_mtu_ut
{
    using namespace swss;
    using namespace std;

    static const string VLAN = "Vlan100";
    static const string SET_MTU_CMD = "/sbin/ip link set Vlan100 mtu ";

    /* Exit status of "ip link set Vlan100 mtu ..." (the kernel refuses an mtu above the Bridge's) */
    static int mock_set_mtu_rc;

    static int cb(const string &cmd, string &stdout)
    {
        mockCallArgs.push_back(cmd);
        if (cmd.find(SET_MTU_CMD) != string::npos && mock_set_mtu_rc != 0)
        {
            stdout = "RTNETLINK answers: Numerical result out of range";
            return mock_set_mtu_rc;
        }
        return 0;
    }

    static vector<string> mtuCommands()
    {
        vector<string> cmds;
        for (const auto &c : mockCallArgs)
        {
            if (c.find(SET_MTU_CMD) != string::npos)
            {
                cmds.push_back(c);
            }
        }
        return cmds;
    }

    struct VlanMgrMtuTest : public ::testing::Test
    {
        shared_ptr<DBConnector> m_config_db;
        shared_ptr<DBConnector> m_app_db;
        shared_ptr<DBConnector> m_state_db;
        shared_ptr<VlanMgr> m_vlanMgr;
        shared_ptr<Table> m_appVlanTable;
        Consumer *m_vlanConsumer;

        virtual void SetUp() override
        {
            testing_db::reset();
            m_config_db = make_shared<DBConnector>("CONFIG_DB", 0);
            m_app_db = make_shared<DBConnector>("APPL_DB", 0);
            m_state_db = make_shared<DBConnector>("STATE_DB", 0);
            m_appVlanTable = make_shared<Table>(m_app_db.get(), APP_VLAN_TABLE_NAME);

            gMacAddress = MacAddress("00:11:22:33:44:55");
            mockCallArgs.clear();
            mock_set_mtu_rc = 0;
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
            m_vlanConsumer = dynamic_cast<Consumer *>(m_vlanMgr->getExecutor(CFG_VLAN_TABLE_NAME));
            ASSERT_NE(m_vlanConsumer, nullptr);
            /* Forget the bridge setup commands the constructor ran */
            mockCallArgs.clear();
        }

        virtual void TearDown() override
        {
            callback = nullptr;
        }

        /* A CONFIG_DB VLAN update as the subscriber delivers it: the whole hash */
        void setVlan(const vector<FieldValueTuple> &fvs)
        {
            deque<KeyOpFieldsValuesTuple> entries;
            entries.push_back({ VLAN, SET_COMMAND, fvs });
            m_vlanConsumer->addToSync(entries);
            m_vlanMgr->doTask();
            EXPECT_EQ(m_vlanConsumer->m_toSync.size(), 0);
        }

        string appMtu()
        {
            string mtu;
            m_appVlanTable->hget(VLAN, "mtu", mtu);
            return mtu;
        }
    };

    /* The VLAN's mtu reaches the kernel Vlan interface when it is created */
    TEST_F(VlanMgrMtuTest, MtuSetOnCreate)
    {
        setVlan({ { "vlanid", "100" }, { "mtu", "1600" } });

        EXPECT_EQ(mtuCommands(), vector<string>({ SET_MTU_CMD + "1600" }));
        EXPECT_EQ(appMtu(), "1600");
    }

    /* A runtime mtu change of an existing VLAN reaches the kernel too (buildimage #174) */
    TEST_F(VlanMgrMtuTest, MtuChangeOnExistingVlan)
    {
        setVlan({ { "vlanid", "100" } });
        EXPECT_EQ(mtuCommands(), vector<string>({ SET_MTU_CMD + "9100" }));
        EXPECT_EQ(appMtu(), "9100");

        for (auto mtu : { "1600", "9000", "9100" })
        {
            mockCallArgs.clear();
            setVlan({ { "vlanid", "100" }, { "mtu", mtu } });
            /* The VLAN is not created again */
            for (const auto &c : mockCallArgs)
            {
                EXPECT_EQ(c.find("type vlan id"), string::npos) << c;
            }
            EXPECT_EQ(mtuCommands(), vector<string>({ SET_MTU_CMD + mtu }));
            EXPECT_EQ(appMtu(), mtu);
        }
    }

    /* Removing the mtu field puts the VLAN back to the default, like APPL_DB */
    TEST_F(VlanMgrMtuTest, MtuRemovedRestoresDefault)
    {
        setVlan({ { "vlanid", "100" }, { "mtu", "1600" } });
        mockCallArgs.clear();
        setVlan({ { "vlanid", "100" } });

        EXPECT_EQ(mtuCommands(), vector<string>({ SET_MTU_CMD + "9100" }));
        EXPECT_EQ(appMtu(), "9100");
    }

    /* An mtu the kernel refuses (above the Bridge's) is logged, not retried,
     * and doesn't stop the VLAN from being processed */
    TEST_F(VlanMgrMtuTest, MtuRefusedByKernel)
    {
        mock_set_mtu_rc = 2;
        setVlan({ { "vlanid", "100" }, { "mtu", "9216" } });

        EXPECT_EQ(mtuCommands(), vector<string>({ SET_MTU_CMD + "9216" }));
        EXPECT_EQ(appMtu(), "9216");
        EXPECT_EQ(m_vlanMgr->m_vlans.count(VLAN), 1);
    }

    /* A malformed mtu never reaches the shell */
    TEST_F(VlanMgrMtuTest, InvalidMtuNotApplied)
    {
        for (auto mtu : { "abc", "1600; reboot", "-1", "0", "99999999999", "+1600", " 1600", "" })
        {
            mockCallArgs.clear();
            setVlan({ { "vlanid", "100" }, { "mtu", mtu } });
            EXPECT_TRUE(mtuCommands().empty()) << mtu;
            EXPECT_TRUE(mockCallArgs.empty()) << mtu;
            EXPECT_TRUE(appMtu().empty()) << mtu;
            EXPECT_EQ(m_vlanMgr->m_vlans.count(VLAN), 0);
        }
    }
    TEST_F(VlanMgrMtuTest, InvalidMtuPreservesAcceptedState)
    {
        setVlan({ { "vlanid", "100" }, { "mtu", "1600" } });
        mockCallArgs.clear();
        setVlan({ { "vlanid", "100" }, { "mtu", "invalid" } });
        EXPECT_TRUE(mockCallArgs.empty());
        EXPECT_EQ(appMtu(), "1600");
        EXPECT_EQ(m_vlanMgr->m_vlans.count(VLAN), 1);
    }

}
