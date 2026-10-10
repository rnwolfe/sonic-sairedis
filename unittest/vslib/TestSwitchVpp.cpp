#include "vpp/SwitchVpp.h"

#include "meta/sai_serialize.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace saivs;

/*
 * Recording stubs for the VPP API. The unit test build links the test binary
 * with the linker's --wrap option for each of them, so SwitchVpp calls land
 * here instead of on a VPP socket.
 */
namespace
{
    struct VppCall
    {
        std::string api;
        std::string name;   // interface or table name
        uint32_t id;        // table id, VNI or instance
        uint32_t sub_id;
        bool flag;          // is_ipv6 or is_add
    };

    std::vector<VppCall> g_vppCalls;

    struct VppRoutePath
    {
        std::string hwif_name;
        uint32_t sw_if_index;
    };

    struct VppRoute
    {
        uint32_t vrf_id;
        bool is_add;
        std::vector<VppRoutePath> paths;
    };

    std::vector<VppRoute> g_vppRoutes;

    uint32_t g_nextSwIfIndex = 100;

    // destination address of each VXLAN tunnel the stub created, by sw_if_index
    std::map<uint32_t, std::string> g_vxlanTunnelDst;

    /*
     * VPP sends L2_MACS_EVENT on the command socket. These are the events it
     * has sent that nothing has read yet; vpp_l2_macs_events_poll() hands them
     * to the callback the switch registered with vpp_want_l2_macs_events2().
     * No other stub reads them, so the tests see a SONiC that makes no
     * unrelated VPP call.
     */
    std::vector<vpp_mac_event_t> g_unreadMacEvents;
    vpp_mac_event_cb_fn g_macEventCb = nullptr;
    void *g_macEventCtx = nullptr;

    std::string macStr(
            const uint8_t *mac)
    {
        SWSS_LOG_ENTER();

        char buf[18];

        snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        return buf;
    }

    // position of the first recorded call matching api and name, or -1
    int vppCallIndex(
            const std::string& api,
            const std::string& name)
    {
        SWSS_LOG_ENTER();

        for (size_t i = 0; i < g_vppCalls.size(); i++)
        {
            if (g_vppCalls[i].api == api && g_vppCalls[i].name == name)
            {
                return (int)i;
            }
        }

        return -1;
    }

    std::vector<VppCall> vppCallsTo(
            const std::string& api)
    {
        SWSS_LOG_ENTER();

        std::vector<VppCall> calls;

        for (auto& call: g_vppCalls)
        {
            if (call.api == api)
            {
                calls.push_back(call);
            }
        }

        return calls;
    }
}

extern "C" {

int __wrap_init_vpp_client() { return 0; }
int __wrap_vpp_sync_for_events() { return 0; }
vpp_event_info_t * __wrap_vpp_ev_dequeue() { return NULL; }
int __wrap_vpp_want_l2_macs_events2(bool enable, vpp_mac_event_cb_fn cb, void *ctx)
{
    SWSS_LOG_ENTER();

    g_macEventCb = enable ? cb : nullptr;
    g_macEventCtx = enable ? ctx : nullptr;
    return 0;
}

int __wrap_vpp_l2fib_set_scan_delay(uint16_t delay_10ms) { return 0; }

int __wrap_vpp_l2_macs_events_poll()
{
    SWSS_LOG_ENTER();

    g_vppCalls.push_back({"vpp_l2_macs_events_poll", "", 0, 0, false});

    if (g_unreadMacEvents.empty())
    {
        return 0;
    }

    std::vector<vpp_mac_event_t> evs;

    evs.swap(g_unreadMacEvents);

    if (g_macEventCb)
    {
        g_macEventCb(evs.data(), (uint32_t)evs.size(), g_macEventCtx);
    }

    return 1;
}
int __wrap_refresh_interfaces_list() { return 0; }
int __wrap_set_sw_interface_l2_bridge(const char *hwif_name, uint32_t bridge_id, bool l2_enable, uint32_t port_type)
{
    SWSS_LOG_ENTER();

    g_vppCalls.push_back({"set_sw_interface_l2_bridge", hwif_name ? hwif_name : "", bridge_id, port_type, l2_enable});
    return 0;
}

int __wrap_set_sw_interface_l2_bridge_by_index(uint32_t sw_if_index, uint32_t bridge_id, bool l2_enable, uint32_t port_type)
{
    SWSS_LOG_ENTER();

    // split horizon group 0
    g_vppCalls.push_back({"set_sw_interface_l2_bridge_shg_by_index", std::to_string(sw_if_index), bridge_id, 0, l2_enable});
    return 0;
}

int __wrap_set_sw_interface_l2_bridge_shg_by_index(uint32_t sw_if_index, uint32_t bridge_id, bool l2_enable, uint32_t port_type, uint8_t shg)
{
    SWSS_LOG_ENTER();

    g_vppCalls.push_back({"set_sw_interface_l2_bridge_shg_by_index", std::to_string(sw_if_index), bridge_id, shg, l2_enable});
    return 0;
}

int __wrap_set_sw_interface_l2_flags_by_index(uint32_t sw_if_index, uint32_t flags, bool enable)
{
    SWSS_LOG_ENTER();

    g_vppCalls.push_back({"set_sw_interface_l2_flags_by_index", std::to_string(sw_if_index), flags, 0, enable});
    return 0;
}

int __wrap_interface_set_state(const char *hwif_name, bool is_up) { return 0; }
int __wrap_configure_lcp_interface(const char *hwif_name, const char *hostif_name, bool is_add)
{
    SWSS_LOG_ENTER();

    g_vppCalls.push_back({"configure_lcp_interface",
            std::string(hwif_name ? hwif_name : "") + " " + (hostif_name ? hostif_name : ""), 0, 0, is_add});
    return 0;
}

int __wrap_interface_ip_address_add_del(const char *hw_ifname, vpp_ip_route_t *prefix, bool is_add) { return 0; }
int __wrap_vpp_bridge_domain_add_del(uint32_t bridge_id, bool is_add) { return 0; }

int __wrap_ip4_nbr_add_del(const char *hwif_name, uint32_t sw_if_index, struct sockaddr_in *addr,
        bool is_static, bool no_fib_entry, uint8_t *mac, bool is_add)
{
    SWSS_LOG_ENTER();

    g_vppCalls.push_back({"ip4_nbr_add_del", macStr(mac), sw_if_index, 0, is_add});
    return 0;
}

int __wrap_ip6_nbr_add_del(const char *hwif_name, uint32_t sw_if_index, struct sockaddr_in6 *addr,
        bool is_static, bool no_fib_entry, uint8_t *mac, bool is_add)
{
    SWSS_LOG_ENTER();

    return 0;
}

int __wrap_ip_vrf_add(uint32_t vrf_id, const char *vrf_name, bool is_ipv6)
{
    SWSS_LOG_ENTER();

    g_vppCalls.push_back({"ip_vrf_add", vrf_name ? vrf_name : "", vrf_id, 0, is_ipv6});
    return 0;
}

int __wrap_ip_vrf_del(uint32_t vrf_id, const char *vrf_name, bool is_ipv6)
{
    SWSS_LOG_ENTER();

    g_vppCalls.push_back({"ip_vrf_del", vrf_name ? vrf_name : "", vrf_id, 0, is_ipv6});
    return 0;
}

int __wrap_vpp_ip_flow_hash_set(uint32_t vrf_id, uint32_t mask, int addr_family)
{
    SWSS_LOG_ENTER();

    g_vppCalls.push_back({"vpp_ip_flow_hash_set", "", vrf_id, 0, addr_family == AF_INET6});
    return 0;
}

int __wrap_set_interface_vrf(const char *hwif_name, uint32_t sub_id, uint32_t vrf_id, bool is_ipv6)
{
    SWSS_LOG_ENTER();

    g_vppCalls.push_back({"set_interface_vrf", hwif_name ? hwif_name : "", vrf_id, sub_id, is_ipv6});
    return 0;
}

int __wrap_create_bvi_interface(uint8_t *mac_address, uint32_t instance)
{
    SWSS_LOG_ENTER();

    g_vppCalls.push_back({"create_bvi_interface", macStr(mac_address), instance, 0, true});
    return 0;
}

int __wrap_delete_bvi_interface(const char *hwif_name)
{
    SWSS_LOG_ENTER();

    g_vppCalls.push_back({"delete_bvi_interface", hwif_name ? hwif_name : "", 0, 0, false});
    return 0;
}

int __wrap_sw_interface_set_mac(const char *hwif_name, uint8_t *mac_address)
{
    SWSS_LOG_ENTER();

    g_vppCalls.push_back({"sw_interface_set_mac", std::string(hwif_name ? hwif_name : "") + " " + macStr(mac_address), 0, 0, true});
    return 0;
}

int __wrap_sw_interface_set_mac_by_index(uint32_t sw_if_index, uint8_t *mac_address)
{
    SWSS_LOG_ENTER();

    g_vppCalls.push_back({"sw_interface_set_mac_by_index", macStr(mac_address), sw_if_index, 0, true});
    return 0;
}

int __wrap_vpp_vxlan_tunnel_add_del(vpp_vxlan_tunnel_t *tunnel, bool is_add, uint32_t *sw_if_index)
{
    SWSS_LOG_ENTER();

    if (is_add)
    {
        *sw_if_index = g_nextSwIfIndex++;
    }

    // name: "decap_any" for a source-independent decap; sub_id: the
    // sw_if_index a create hands out
    g_vppCalls.push_back({"vpp_vxlan_tunnel_add_del", tunnel->decap_any ? "decap_any" : "",
                          tunnel->vni, is_add ? *sw_if_index : 0, is_add});

    if (is_add)
    {
        char dst[INET6_ADDRSTRLEN];

        vpp_ip_addr_t_to_string(&tunnel->dst_address, dst, sizeof(dst));
        g_vxlanTunnelDst[*sw_if_index] = dst;
    }

    return 0;
}

int __wrap_ip_route_add_del_get_stats(vpp_ip_route_t *prefix, bool is_add, uint32_t *stats_index)
{
    SWSS_LOG_ENTER();

    VppRoute route = { prefix->vrf_id, is_add, {} };

    for (unsigned int i = 0; i < prefix->nexthop_cnt; i++)
    {
        auto& nh = prefix->nexthop[i];

        route.paths.push_back({nh.hwif_name ? nh.hwif_name : "", nh.sw_if_index});
    }

    g_vppRoutes.push_back(route);

    if (stats_index)
    {
        *stats_index = 7;
    }

    return 0;
}

int __wrap_l2fib_add_del(const char *hwif_name, const uint8_t *mac, uint32_t bd_id, bool is_add, bool is_static_mac)
{
    SWSS_LOG_ENTER();

    g_vppCalls.push_back({"l2fib_add_del", std::string(hwif_name ? hwif_name : "") + " " + macStr(mac), bd_id, is_static_mac, is_add});
    return 0;
}

int __wrap_l2fib_add_del_by_index(uint32_t sw_if_index, const uint8_t *mac, uint32_t bd_id, bool is_add, bool is_static_mac)
{
    SWSS_LOG_ENTER();

    g_vppCalls.push_back({"l2fib_add_del_by_index", std::to_string(sw_if_index) + " " + macStr(mac), bd_id, is_static_mac, is_add});
    return 0;
}

int __wrap_l2fib_flush_all()
{
    SWSS_LOG_ENTER();

    g_vppCalls.push_back({"l2fib_flush_all", "", 0, 0, false});
    return 0;
}

int __wrap_l2fib_flush_int(const char *hwif_name)
{
    SWSS_LOG_ENTER();

    g_vppCalls.push_back({"l2fib_flush_int", hwif_name ? hwif_name : "", 0, 0, false});
    return 0;
}

int __wrap_l2fib_flush_bd(uint32_t bd_id)
{
    SWSS_LOG_ENTER();

    g_vppCalls.push_back({"l2fib_flush_bd", "", bd_id, 0, false});
    return 0;
}

}

TEST(SwitchVpp, getLagMemberEgressDisableAction)
{
    using Action = SwitchVpp::LagMemberEgressDisableAction;

    EXPECT_EQ(Action::NONE, SwitchVpp::getLagMemberEgressDisableAction(false, true, false));
    EXPECT_EQ(Action::DISABLE, SwitchVpp::getLagMemberEgressDisableAction(true, true, false));
    EXPECT_EQ(Action::ENABLE, SwitchVpp::getLagMemberEgressDisableAction(false, true, true));
    EXPECT_EQ(Action::NONE, SwitchVpp::getLagMemberEgressDisableAction(true, true, true));
    EXPECT_EQ(Action::NONE, SwitchVpp::getLagMemberEgressDisableAction(false, false, false));
    EXPECT_EQ(Action::DISABLE, SwitchVpp::getLagMemberEgressDisableAction(true, false, false));
}

/*
 * The unit tests are built with access control checking disabled, so the
 * fixture reaches the protected and private SwitchVpp members directly.
 */
class SwitchVppVrf : public ::testing::Test
{
    protected:

        void SetUp() override
        {
            auto sc = std::make_shared<SwitchConfig>(0, "");

            sc->m_saiSwitchType = SAI_SWITCH_TYPE_NPU;
            sc->m_switchType = SAI_VS_SWITCH_TYPE_VPP;
            sc->m_bootType = SAI_VS_BOOT_TYPE_COLD;
            sc->m_useTapDevice = true;
            sc->m_laneMap = LaneMap::getDefaultLaneMap(0);
            sc->m_eventQueue = std::make_shared<EventQueue>(std::make_shared<Signal>());

            auto scc = std::make_shared<SwitchConfigContainer>();

            scc->insert(sc);

            m_mgr = std::make_shared<RealObjectIdManager>(0, scc);
            m_sw = std::make_shared<SwitchVpp>(m_switchId, m_mgr, sc);

            ASSERT_EQ(SAI_STATUS_SUCCESS, m_sw->create_default_virtual_router());

            sai_attribute_t attr;

            attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;

            ASSERT_EQ(SAI_STATUS_SUCCESS, m_sw->get(SAI_OBJECT_TYPE_SWITCH, sai_serialize_object_id(m_switchId), 1, &attr));

            m_defaultVr = attr.value.oid;

            g_vppCalls.clear();
            g_vppRoutes.clear();
        }

        void TearDown() override
        {
            m_sw.reset();
        }

        sai_object_id_t createVr()
        {
            SWSS_LOG_ENTER();

            auto vr = m_mgr->allocateNewObjectId(SAI_OBJECT_TYPE_VIRTUAL_ROUTER, m_switchId);

            EXPECT_EQ(SAI_STATUS_SUCCESS,
                    m_sw->create(SAI_OBJECT_TYPE_VIRTUAL_ROUTER, sai_serialize_object_id(vr), m_switchId, 0, nullptr));

            return vr;
        }

        // A virtual router that exists in SAI but was never given a VPP table.
        sai_object_id_t createVrWithoutTable()
        {
            SWSS_LOG_ENTER();

            auto vr = m_mgr->allocateNewObjectId(SAI_OBJECT_TYPE_VIRTUAL_ROUTER, m_switchId);

            EXPECT_EQ(SAI_STATUS_SUCCESS,
                    m_sw->create_internal(SAI_OBJECT_TYPE_VIRTUAL_ROUTER, sai_serialize_object_id(vr), m_switchId, 0, nullptr));

            return vr;
        }

        uint32_t tableOf(
                sai_object_id_t vr)
        {
            SWSS_LOG_ENTER();

            auto vrf = m_sw->vpp_get_ip_vrf(vr);

            EXPECT_NE(nullptr, vrf);

            return vrf ? vrf->m_vrf_id : UINT32_MAX;
        }

        sai_status_t createVlanRif(
                sai_object_id_t vr,
                uint16_t vlanId,
                sai_object_id_t *rifOut = nullptr)
        {
            SWSS_LOG_ENTER();

            auto vlan = m_mgr->allocateNewObjectId(SAI_OBJECT_TYPE_VLAN, m_switchId);

            sai_attribute_t vattr;

            vattr.id = SAI_VLAN_ATTR_VLAN_ID;
            vattr.value.u16 = vlanId;

            EXPECT_EQ(SAI_STATUS_SUCCESS,
                    m_sw->create_internal(SAI_OBJECT_TYPE_VLAN, sai_serialize_object_id(vlan), m_switchId, 1, &vattr));

            sai_attribute_t attrs[4];
            sai_mac_t mac = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };

            attrs[0].id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
            attrs[0].value.s32 = SAI_ROUTER_INTERFACE_TYPE_VLAN;
            attrs[1].id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
            attrs[1].value.oid = vr;
            attrs[2].id = SAI_ROUTER_INTERFACE_ATTR_VLAN_ID;
            attrs[2].value.oid = vlan;
            attrs[3].id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
            memcpy(attrs[3].value.mac, mac, sizeof(sai_mac_t));

            auto rif = m_mgr->allocateNewObjectId(SAI_OBJECT_TYPE_ROUTER_INTERFACE, m_switchId);

            if (rifOut)
            {
                *rifOut = rif;
            }

            return m_sw->create(SAI_OBJECT_TYPE_ROUTER_INTERFACE, sai_serialize_object_id(rif), m_switchId, 4, attrs);
        }

        const sai_object_id_t m_switchId = 0x2100000000;

        std::shared_ptr<RealObjectIdManager> m_mgr;

        std::shared_ptr<SwitchVpp> m_sw;

        sai_object_id_t m_defaultVr = SAI_NULL_OBJECT_ID;
};

TEST_F(SwitchVppVrf, DefaultVirtualRouterUsesTableZero)
{
    ASSERT_EQ(SAI_STATUS_SUCCESS, createVlanRif(m_defaultVr, 100));

    EXPECT_TRUE(vppCallsTo("ip_vrf_add").empty());
    EXPECT_TRUE(vppCallsTo("set_interface_vrf").empty());

    EXPECT_EQ(0u, tableOf(m_defaultVr));

    // registering the default table is what sets its flow hash
    auto hashes = vppCallsTo("vpp_ip_flow_hash_set");

    ASSERT_EQ(2u, hashes.size());
    EXPECT_EQ(0u, hashes[0].id);
    EXPECT_EQ(0u, hashes[1].id);
}

TEST_F(SwitchVppVrf, VirtualRouterGetsItsOwnTable)
{
    const uint32_t base = SwitchVpp::vrf_table_id_base;

    auto vr1 = createVr();
    auto vr2 = createVr();

    EXPECT_EQ(base, tableOf(vr1));
    EXPECT_EQ(base + 1, tableOf(vr2));

    // both address families, named after the table id
    auto adds = vppCallsTo("ip_vrf_add");

    ASSERT_EQ(4u, adds.size());
    EXPECT_EQ(base, adds[0].id);
    EXPECT_FALSE(adds[0].flag);
    EXPECT_EQ("vrf_" + std::to_string(base), adds[0].name);
    EXPECT_EQ(base, adds[1].id);
    EXPECT_TRUE(adds[1].flag);
    EXPECT_EQ("vrf_" + std::to_string(base), m_sw->vpp_get_ip_vrf(vr1)->m_vrf_name);
}

TEST_F(SwitchVppVrf, VlanRouterInterfaceBindsBviToItsVirtualRoutersTable)
{
    auto vr = createVr();
    uint32_t table = tableOf(vr);

    g_vppCalls.clear();

    ASSERT_EQ(SAI_STATUS_SUCCESS, createVlanRif(vr, 100));

    ASSERT_EQ(1u, vppCallsTo("create_bvi_interface").size());

    auto binds = vppCallsTo("set_interface_vrf");

    ASSERT_EQ(2u, binds.size());

    for (auto& bind: binds)
    {
        EXPECT_EQ("bvi100", bind.name);
        EXPECT_EQ(0u, bind.sub_id);
        EXPECT_EQ(table, bind.id);
    }

    EXPECT_FALSE(binds[0].flag);
    EXPECT_TRUE(binds[1].flag);

    // the table was created with the virtual router, not with the interface
    EXPECT_TRUE(vppCallsTo("ip_vrf_add").empty());
}

TEST_F(SwitchVppVrf, VlanRouterInterfaceSourceMacUpdateReachesItsBvi)
{
    sai_object_id_t rif;

    ASSERT_EQ(SAI_STATUS_SUCCESS, createVlanRif(m_defaultVr, 3310, &rif));

    g_vppCalls.clear();

    // orchagent applies the static anycast gateway MAC after the create
    sai_attribute_t attr;

    attr.id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
    const uint8_t gw[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
    memcpy(attr.value.mac, gw, sizeof(gw));

    ASSERT_EQ(SAI_STATUS_SUCCESS, m_sw->set(SAI_OBJECT_TYPE_ROUTER_INTERFACE, sai_serialize_object_id(rif), &attr));

    auto calls = vppCallsTo("sw_interface_set_mac");

    ASSERT_EQ(1u, calls.size());
    EXPECT_EQ("bvi3310 00:00:00:00:00:01", calls[0].name);
}

TEST_F(SwitchVppVrf, SubPortBindsToTheAllocatedTable)
{
    auto vr = createVr();
    uint32_t table = tableOf(vr);

    g_vppCalls.clear();

    // the port and sub-port router interface path
    EXPECT_EQ(SAI_STATUS_SUCCESS, m_sw->vpp_router_interface_set_vrf(vr, "GigabitEthernet0/8/0", 100, "Ethernet0.100"));

    auto binds = vppCallsTo("set_interface_vrf");

    ASSERT_EQ(2u, binds.size());
    EXPECT_EQ("GigabitEthernet0/8/0", binds[0].name);
    EXPECT_EQ(100u, binds[0].sub_id);
    EXPECT_EQ(table, binds[0].id);
    EXPECT_EQ(table, binds[1].id);
}

TEST_F(SwitchVppVrf, UnknownVirtualRouterIsNotCachedAsTableZero)
{
    // No VPP table and no kernel VRF to fall back to (the host interface does
    // not exist here): the interface stays in table 0, but the virtual router
    // must not be remembered as table 0 for later interfaces.
    auto vr = createVrWithoutTable();

    EXPECT_EQ(SAI_STATUS_SUCCESS, m_sw->vpp_router_interface_set_vrf(vr, "bvi200", 0, "Vlan200"));

    EXPECT_TRUE(vppCallsTo("set_interface_vrf").empty());
    EXPECT_EQ(nullptr, m_sw->vpp_get_ip_vrf(vr));
}

TEST_F(SwitchVppVrf, RemovingVirtualRouterReleasesItsTable)
{
    auto vr1 = createVr();
    auto vr2 = createVr();
    uint32_t table1 = tableOf(vr1);

    g_vppCalls.clear();

    ASSERT_EQ(SAI_STATUS_SUCCESS, m_sw->remove(SAI_OBJECT_TYPE_VIRTUAL_ROUTER, sai_serialize_object_id(vr1)));

    auto dels = vppCallsTo("ip_vrf_del");

    ASSERT_EQ(2u, dels.size());
    EXPECT_EQ(table1, dels[0].id);
    EXPECT_FALSE(dels[0].flag);
    EXPECT_EQ(table1, dels[1].id);
    EXPECT_TRUE(dels[1].flag);

    EXPECT_EQ(nullptr, m_sw->vpp_get_ip_vrf(vr1));
    EXPECT_NE(nullptr, m_sw->vpp_get_ip_vrf(vr2));

    // the released id is handed out again
    auto vr3 = createVr();

    EXPECT_EQ(table1, tableOf(vr3));
}

TEST_F(SwitchVppVrf, LinuxNlKeepsKernelTableIds)
{
    // NO_LINUX_NL=n: the linux control plane plugin syncs routes into tables
    // keyed by the kernel table id, so saivpp must not allocate one of its own.
    m_sw->nbr_env_read = true;
    m_sw->nbr_active = false;

    auto vr = createVr();

    EXPECT_TRUE(vppCallsTo("ip_vrf_add").empty());
    EXPECT_EQ(nullptr, m_sw->vpp_get_ip_vrf(vr));
}

/*
 * One VXLAN tunnel with a VR->VNI encap mapper holding three entries: two
 * tenant virtual routers with tables, and one virtual router without.
 */
class SwitchVppTunnelNexthop : public SwitchVppVrf
{
    protected:

        void SetUp() override
        {
            SwitchVppVrf::SetUp();

            if (HasFatalFailure())
            {
                return;
            }

            m_vrA = createVr();
            m_vrB = createVr();
            m_vrUnknown = createVrWithoutTable();

            auto map = m_mgr->allocateNewObjectId(SAI_OBJECT_TYPE_TUNNEL_MAP, m_switchId);
            m_map = map;

            sai_attribute_t mattr;

            mattr.id = SAI_TUNNEL_MAP_ATTR_TYPE;
            mattr.value.s32 = SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI;

            ASSERT_EQ(SAI_STATUS_SUCCESS,
                    m_sw->create_internal(SAI_OBJECT_TYPE_TUNNEL_MAP, sai_serialize_object_id(map), m_switchId, 1, &mattr));

            m_entryA = addMapEntry(map, m_vrA, 1000);
            addMapEntry(map, m_vrB, 2000);
            addMapEntry(map, m_vrUnknown, 3000);

            m_tunnel = m_mgr->allocateNewObjectId(SAI_OBJECT_TYPE_TUNNEL, m_switchId);

            sai_attribute_t tattrs[3];

            tattrs[0].id = SAI_TUNNEL_ATTR_TYPE;
            tattrs[0].value.s32 = SAI_TUNNEL_TYPE_VXLAN;
            tattrs[1].id = SAI_TUNNEL_ATTR_ENCAP_SRC_IP;
            tattrs[1].value.ipaddr = ipv4("10.0.0.1");
            tattrs[2].id = SAI_TUNNEL_ATTR_ENCAP_MAPPERS;
            tattrs[2].value.objlist.count = 1;
            tattrs[2].value.objlist.list = &map;

            ASSERT_EQ(SAI_STATUS_SUCCESS,
                    m_sw->create_internal(SAI_OBJECT_TYPE_TUNNEL, sai_serialize_object_id(m_tunnel), m_switchId, 3, tattrs));

            // stand-ins for the kernel side of the decap host path
            m_sw->m_tunnel_mgr.m_netdev_master = [this](const std::string& netdev)
            {
                SWSS_LOG_ENTER();

                auto it = m_kernelMasters.find(netdev);

                return it == m_kernelMasters.end() ? std::string() : it->second;
            };

            m_sw->m_tunnel_mgr.m_add_tc_redirect = [this](const std::string& tap, const std::string& dev)
            {
                SWSS_LOG_ENTER();

                m_redirects.push_back(tap + " " + dev);

                return m_redirectStatus;
            };

            g_vppCalls.clear();
        }

        void setSwitchMac(
                const std::array<uint8_t, 6>& mac)
        {
            SWSS_LOG_ENTER();

            sai_attribute_t attr;

            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
            memcpy(attr.value.mac, mac.data(), mac.size());

            ASSERT_EQ(SAI_STATUS_SUCCESS,
                    m_sw->set_internal(SAI_OBJECT_TYPE_SWITCH, sai_serialize_object_id(m_switchId), &attr));
        }

        // name of the next hop's own decap BVI: the first created since the
        // last clear (the decap-only tunnels of the other L3VNIs follow it)
        std::string decapBvi(
                size_t index = 0)
        {
            SWSS_LOG_ENTER();

            auto bvis = vppCallsTo("create_bvi_interface");

            EXPECT_LT(index, bvis.size());

            return bvis.size() <= index ? std::string() : "bvi" + std::to_string(bvis[index].id);
        }

        static sai_ip_address_t ipv4(
                const char *addr)
        {
            SWSS_LOG_ENTER();

            sai_ip_address_t ip;

            memset(&ip, 0, sizeof(ip));
            ip.addr_family = SAI_IP_ADDR_FAMILY_IPV4;

            EXPECT_EQ(1, inet_pton(AF_INET, addr, &ip.addr.ip4));

            return ip;
        }

        sai_object_id_t addMapEntry(
                sai_object_id_t map,
                sai_object_id_t vr,
                uint32_t vni)
        {
            SWSS_LOG_ENTER();

            auto entry = m_mgr->allocateNewObjectId(SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, m_switchId);

            sai_attribute_t eattrs[4];

            eattrs[0].id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE;
            eattrs[0].value.s32 = SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI;
            eattrs[1].id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP;
            eattrs[1].value.oid = map;
            eattrs[2].id = SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_KEY;
            eattrs[2].value.oid = vr;
            eattrs[3].id = SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_VALUE;
            eattrs[3].value.u32 = vni;

            // through create(), so the switch sees it the way syncd hands it over
            EXPECT_EQ(SAI_STATUS_SUCCESS,
                    m_sw->create(SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, sai_serialize_object_id(entry), m_switchId, 4, eattrs));

            return entry;
        }

        sai_status_t createNexthop(
                uint32_t vni,
                sai_object_id_t& nh,
                const uint8_t *remoteRouterMac = nullptr)
        {
            SWSS_LOG_ENTER();

            nh = m_mgr->allocateNewObjectId(SAI_OBJECT_TYPE_NEXT_HOP, m_switchId);

            sai_attribute_t attrs[5];
            uint32_t count = 4;

            attrs[0].id = SAI_NEXT_HOP_ATTR_TYPE;
            attrs[0].value.s32 = SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP;
            attrs[1].id = SAI_NEXT_HOP_ATTR_IP;
            attrs[1].value.ipaddr = ipv4("10.0.0.2");
            attrs[2].id = SAI_NEXT_HOP_ATTR_TUNNEL_ID;
            attrs[2].value.oid = m_tunnel;
            attrs[3].id = SAI_NEXT_HOP_ATTR_TUNNEL_VNI;
            attrs[3].value.u32 = vni;

            if (remoteRouterMac)
            {
                attrs[4].id = SAI_NEXT_HOP_ATTR_TUNNEL_MAC;
                memcpy(attrs[4].value.mac, remoteRouterMac, sizeof(sai_mac_t));
                count = 5;
            }

            return m_sw->create(SAI_OBJECT_TYPE_NEXT_HOP, sai_serialize_object_id(nh), m_switchId, count, attrs);
        }

        sai_object_id_t m_vrA = SAI_NULL_OBJECT_ID;
        sai_object_id_t m_vrB = SAI_NULL_OBJECT_ID;
        sai_object_id_t m_vrUnknown = SAI_NULL_OBJECT_ID;
        sai_object_id_t m_tunnel = SAI_NULL_OBJECT_ID;
        sai_object_id_t m_entryA = SAI_NULL_OBJECT_ID;
        sai_object_id_t m_map = SAI_NULL_OBJECT_ID;

        std::map<std::string, std::string> m_kernelMasters;

        std::vector<std::string> m_redirects;

        sai_status_t m_redirectStatus = SAI_STATUS_SUCCESS;
};

namespace
{
    // what an EVPN type-5 next hop carries: the remote VTEP's router MAC
    const uint8_t REMOTE_ROUTER_MAC[6] = { 0x22, 0x36, 0x29, 0xc2, 0x14, 0xf1 };
}

TEST_F(SwitchVppTunnelNexthop, UsesTheMapEntryOfItsVni)
{
    sai_object_id_t nh;

    // the entry of a virtual router without a table is present, and ignored
    ASSERT_EQ(SAI_STATUS_SUCCESS, createNexthop(2000, nh));

    auto tunnels = vppCallsTo("vpp_vxlan_tunnel_add_del");

    // its own tunnel first, then the decap-only one of VR A's VNI
    ASSERT_EQ(2u, tunnels.size());
    EXPECT_EQ(2000u, tunnels[0].id);
    EXPECT_TRUE(tunnels[0].flag);

    // the decap BVI routes in the table of the VNI's virtual router
    uint32_t table = tableOf(m_vrB);
    auto binds = vppCallsTo("set_interface_vrf");

    ASSERT_EQ(4u, binds.size());
    EXPECT_EQ(table, binds[0].id);
    EXPECT_FALSE(binds[0].flag);
    EXPECT_EQ(table, binds[1].id);
    EXPECT_TRUE(binds[1].flag);

    uint32_t sw_if_index = 0;

    EXPECT_EQ(SAI_STATUS_SUCCESS, m_sw->m_tunnel_mgr.get_tunnel_if(nh, sw_if_index));
}

TEST_F(SwitchVppTunnelNexthop, RemoveTearsDownTheTunnelOfItsVni)
{
    sai_object_id_t nh;

    ASSERT_EQ(SAI_STATUS_SUCCESS, createNexthop(1000, nh));

    g_vppCalls.clear();

    ASSERT_EQ(SAI_STATUS_SUCCESS, m_sw->remove(SAI_OBJECT_TYPE_NEXT_HOP, sai_serialize_object_id(nh)));

    auto tunnels = vppCallsTo("vpp_vxlan_tunnel_add_del");

    // the VTEP's last next hop takes its decap-only tunnels with it
    ASSERT_EQ(2u, tunnels.size());
    EXPECT_EQ(1000u, tunnels[0].id);
    EXPECT_FALSE(tunnels[0].flag);
    EXPECT_EQ(2000u, tunnels[1].id);
    EXPECT_FALSE(tunnels[1].flag);

    EXPECT_EQ(2u, vppCallsTo("delete_bvi_interface").size());

    uint32_t sw_if_index = 0;

    EXPECT_NE(SAI_STATUS_SUCCESS, m_sw->m_tunnel_mgr.get_tunnel_if(nh, sw_if_index));
}

TEST_F(SwitchVppTunnelNexthop, FailsWhenItsVniHasNoUsableVirtualRouter)
{
    sai_object_id_t nh;

    // the VNI's virtual router has no table yet
    EXPECT_NE(SAI_STATUS_SUCCESS, createNexthop(3000, nh));

    // no map entry carries the VNI at all
    EXPECT_NE(SAI_STATUS_SUCCESS, createNexthop(4000, nh));

    EXPECT_TRUE(vppCallsTo("vpp_vxlan_tunnel_add_del").empty());
}

TEST_F(SwitchVppTunnelNexthop, EncapsulatesToTheRouterMacOfTheRemoteVtep)
{
    sai_object_id_t nh;

    ASSERT_EQ(SAI_STATUS_SUCCESS, createNexthop(2000, nh, REMOTE_ROUTER_MAC));

    auto nbrs = vppCallsTo("ip4_nbr_add_del");

    ASSERT_EQ(1u, nbrs.size());
    EXPECT_EQ("22:36:29:c2:14:f1", nbrs[0].name);
    EXPECT_TRUE(nbrs[0].flag);

    g_vppCalls.clear();

    ASSERT_EQ(SAI_STATUS_SUCCESS, m_sw->remove(SAI_OBJECT_TYPE_NEXT_HOP, sai_serialize_object_id(nh)));

    // the remove takes out the neighbour the create added
    nbrs = vppCallsTo("ip4_nbr_add_del");

    ASSERT_EQ(1u, nbrs.size());
    EXPECT_EQ("22:36:29:c2:14:f1", nbrs[0].name);
    EXPECT_FALSE(nbrs[0].flag);
}

TEST_F(SwitchVppTunnelNexthop, DecapBviAnswersToTheSwitchMac)
{
    setSwitchMac({ 0x22, 0xfd, 0xe1, 0x99, 0x49, 0x9e });

    sai_object_id_t nh;

    ASSERT_EQ(SAI_STATUS_SUCCESS, createNexthop(2000, nh, REMOTE_ROUTER_MAC));

    auto bvis = vppCallsTo("create_bvi_interface");

    // the decap-only BVI of VR A's VNI too
    ASSERT_EQ(2u, bvis.size());
    EXPECT_EQ("22:fd:e1:99:49:9e", bvis[0].name);
    EXPECT_EQ("22:fd:e1:99:49:9e", bvis[1].name);
}

TEST_F(SwitchVppTunnelNexthop, WithoutARemoteRouterMacBothSidesKeepTheSharedOne)
{
    setSwitchMac({ 0x22, 0xfd, 0xe1, 0x99, 0x49, 0x9e });

    sai_object_id_t nh;

    ASSERT_EQ(SAI_STATUS_SUCCESS, createNexthop(2000, nh));

    auto nbrs = vppCallsTo("ip4_nbr_add_del");

    ASSERT_EQ(1u, nbrs.size());
    EXPECT_EQ("00:00:00:00:00:01", nbrs[0].name);

    auto bvis = vppCallsTo("create_bvi_interface");

    ASSERT_EQ(2u, bvis.size());
    EXPECT_EQ("00:00:00:00:00:01", bvis[0].name);
    EXPECT_EQ("00:00:00:00:00:01", bvis[1].name);
}

TEST_F(SwitchVppTunnelNexthop, DecapBviGetsAHostPathIntoItsKernelVrf)
{
    ASSERT_EQ(SAI_STATUS_SUCCESS, createVlanRif(m_vrB, 3000));

    m_kernelMasters["Vlan3000"] = "Vrft";

    g_vppCalls.clear();

    sai_object_id_t nh;

    ASSERT_EQ(SAI_STATUS_SUCCESS, createNexthop(2000, nh, REMOTE_ROUTER_MAC));

    auto bvi = decapBvi();

    int pair = vppCallIndex("configure_lcp_interface", bvi + " tap_" + bvi);
    int bridge = vppCallIndex("set_sw_interface_l2_bridge", bvi);

    ASSERT_NE(-1, pair);
    ASSERT_NE(-1, bridge);
    EXPECT_TRUE(g_vppCalls[pair].flag);

    // paired before it becomes its bridge domain's BVI, or sonic_ext steers
    // the punts to the underlay port's tap
    EXPECT_LT(pair, bridge);

    ASSERT_EQ(1u, m_redirects.size());
    EXPECT_EQ("tap_" + bvi + " Vrft", m_redirects[0]);
}

TEST_F(SwitchVppTunnelNexthop, NoKernelVrfMeansNoHostPath)
{
    // the VR has a VLAN router interface, but its SVI is in no kernel VRF
    ASSERT_EQ(SAI_STATUS_SUCCESS, createVlanRif(m_vrB, 3000));

    g_vppCalls.clear();

    sai_object_id_t nh;

    ASSERT_EQ(SAI_STATUS_SUCCESS, createNexthop(2000, nh, REMOTE_ROUTER_MAC));

    EXPECT_TRUE(vppCallsTo("configure_lcp_interface").empty());
    EXPECT_TRUE(m_redirects.empty());
}

TEST_F(SwitchVppTunnelNexthop, DoesNotBorrowTheKernelVrfOfAnotherVirtualRouter)
{
    ASSERT_EQ(SAI_STATUS_SUCCESS, createVlanRif(m_vrA, 100));

    m_kernelMasters["Vlan100"] = "VrfA";

    g_vppCalls.clear();

    sai_object_id_t nh;

    // VNI 2000 belongs to VR B
    ASSERT_EQ(SAI_STATUS_SUCCESS, createNexthop(2000, nh, REMOTE_ROUTER_MAC));

    // neither the next hop's BVI nor VR A's decap-only one (decap-only BVIs
    // get no host path) goes into VR A's kernel VRF
    EXPECT_TRUE(vppCallsTo("configure_lcp_interface").empty());
    EXPECT_TRUE(m_redirects.empty());
}

TEST_F(SwitchVppTunnelNexthop, GivesItsVtepADecapTunnelForEveryOtherL3Vni)
{
    sai_object_id_t nh;

    ASSERT_EQ(SAI_STATUS_SUCCESS, createNexthop(2000, nh, REMOTE_ROUTER_MAC));

    // VR A's VNI gets a tunnel that only decapsulates: a BVI in VR A's table
    // and no neighbour. VNI 3000 has no table and gets nothing.
    auto tunnels = vppCallsTo("vpp_vxlan_tunnel_add_del");

    ASSERT_EQ(2u, tunnels.size());
    EXPECT_EQ(1000u, tunnels[1].id);
    EXPECT_TRUE(tunnels[1].flag);

    // it decapsulates like the next hop's own tunnel (P2MP: from any source),
    // and carries the router MAC a next hop taking it over encapsulates with
    EXPECT_EQ(tunnels[0].name, tunnels[1].name);
    EXPECT_EQ("decap_any", tunnels[1].name);

    auto macs = vppCallsTo("sw_interface_set_mac_by_index");

    ASSERT_EQ(2u, macs.size());
    EXPECT_EQ(tunnels[1].sub_id, macs[1].id);
    EXPECT_EQ(macs[0].name, macs[1].name);

    auto binds = vppCallsTo("set_interface_vrf");

    ASSERT_EQ(4u, binds.size());
    EXPECT_EQ(tableOf(m_vrA), binds[2].id);
    EXPECT_EQ(tableOf(m_vrA), binds[3].id);

    EXPECT_EQ(1u, vppCallsTo("ip4_nbr_add_del").size());
}

TEST_F(SwitchVppTunnelNexthop, DecapOnlyTunnelsGetNoHostPath)
{
    ASSERT_EQ(SAI_STATUS_SUCCESS, createVlanRif(m_vrA, 100));
    ASSERT_EQ(SAI_STATUS_SUCCESS, createVlanRif(m_vrB, 3000));

    m_kernelMasters["Vlan100"] = "VrfA";
    m_kernelMasters["Vlan3000"] = "Vrft";

    g_vppCalls.clear();

    sai_object_id_t nh;

    ASSERT_EQ(SAI_STATUS_SUCCESS, createNexthop(2000, nh, REMOTE_ROUTER_MAC));

    // only the next hop's BVI pairs a host tap: each tap pins VPP buffers
    auto own = decapBvi(0);

    ASSERT_EQ(1u, vppCallsTo("configure_lcp_interface").size());
    ASSERT_EQ(1u, m_redirects.size());
    EXPECT_EQ("tap_" + own + " Vrft", m_redirects[0]);
}

TEST_F(SwitchVppTunnelNexthop, ALaterNexthopTakesOverTheDecapTunnelOfItsVni)
{
    sai_object_id_t nhB;

    ASSERT_EQ(SAI_STATUS_SUCCESS, createNexthop(2000, nhB, REMOTE_ROUTER_MAC));

    uint32_t decapOnly = vppCallsTo("vpp_vxlan_tunnel_add_del")[1].sub_id;

    g_vppCalls.clear();

    sai_object_id_t nhA;

    ASSERT_EQ(SAI_STATUS_SUCCESS, createNexthop(1000, nhA, REMOTE_ROUTER_MAC));

    // no new tunnel, but the decap side is rebuilt so the next hop's BVI gets
    // the host path the decap-only one went without, plus the encap neighbour
    EXPECT_TRUE(vppCallsTo("vpp_vxlan_tunnel_add_del").empty());
    EXPECT_EQ(1u, vppCallsTo("delete_bvi_interface").size());
    EXPECT_EQ(1u, vppCallsTo("create_bvi_interface").size());

    auto nbrs = vppCallsTo("ip4_nbr_add_del");

    ASSERT_EQ(1u, nbrs.size());
    EXPECT_EQ("22:36:29:c2:14:f1", nbrs[0].name);
    EXPECT_EQ(decapOnly, nbrs[0].id);
    EXPECT_TRUE(nbrs[0].flag);

    uint32_t sw_if_index = 0;

    EXPECT_EQ(SAI_STATUS_SUCCESS, m_sw->m_tunnel_mgr.get_tunnel_if(nhA, sw_if_index));
    EXPECT_EQ(decapOnly, sw_if_index);
}

TEST_F(SwitchVppTunnelNexthop, KeepsATunnelForDecapWhileItsVtepHasOtherNexthops)
{
    sai_object_id_t nhA, nhB;

    ASSERT_EQ(SAI_STATUS_SUCCESS, createNexthop(2000, nhB, REMOTE_ROUTER_MAC));
    ASSERT_EQ(SAI_STATUS_SUCCESS, createNexthop(1000, nhA, REMOTE_ROUTER_MAC));

    g_vppCalls.clear();

    // the VTEP still has nhA, so VNI 2000 keeps decapsulating
    ASSERT_EQ(SAI_STATUS_SUCCESS, m_sw->remove(SAI_OBJECT_TYPE_NEXT_HOP, sai_serialize_object_id(nhB)));

    EXPECT_TRUE(vppCallsTo("vpp_vxlan_tunnel_add_del").empty());
    EXPECT_TRUE(vppCallsTo("delete_bvi_interface").empty());

    auto nbrs = vppCallsTo("ip4_nbr_add_del");

    ASSERT_EQ(1u, nbrs.size());
    EXPECT_FALSE(nbrs[0].flag);

    g_vppCalls.clear();

    // its last next hop gone, every tunnel to the VTEP goes
    ASSERT_EQ(SAI_STATUS_SUCCESS, m_sw->remove(SAI_OBJECT_TYPE_NEXT_HOP, sai_serialize_object_id(nhA)));

    auto tunnels = vppCallsTo("vpp_vxlan_tunnel_add_del");

    ASSERT_EQ(2u, tunnels.size());
    EXPECT_EQ(1000u, tunnels[0].id);
    EXPECT_FALSE(tunnels[0].flag);
    EXPECT_EQ(2000u, tunnels[1].id);
    EXPECT_FALSE(tunnels[1].flag);
    EXPECT_EQ(2u, vppCallsTo("delete_bvi_interface").size());
}

TEST_F(SwitchVppTunnelNexthop, AVrfAddedLaterGetsADecapTunnelToEveryKnownVtep)
{
    sai_object_id_t nh;

    ASSERT_EQ(SAI_STATUS_SUCCESS, createNexthop(2000, nh, REMOTE_ROUTER_MAC));

    auto vrC = createVr();

    g_vppCalls.clear();

    // a VRF and its L3VNI configured after the fabric converged
    addMapEntry(m_map, vrC, 4000);

    auto tunnels = vppCallsTo("vpp_vxlan_tunnel_add_del");

    ASSERT_EQ(1u, tunnels.size());
    EXPECT_EQ(4000u, tunnels[0].id);
    EXPECT_TRUE(tunnels[0].flag);

    auto binds = vppCallsTo("set_interface_vrf");

    ASSERT_EQ(2u, binds.size());
    EXPECT_EQ(tableOf(vrC), binds[0].id);
}

TEST_F(SwitchVppTunnelNexthop, RemovingAVniMapEntryRemovesTheDecapTunnelsOfItsVni)
{
    sai_object_id_t nh;

    ASSERT_EQ(SAI_STATUS_SUCCESS, createNexthop(2000, nh, REMOTE_ROUTER_MAC));

    g_vppCalls.clear();

    ASSERT_EQ(SAI_STATUS_SUCCESS, m_sw->remove(SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, sai_serialize_object_id(m_entryA)));

    auto tunnels = vppCallsTo("vpp_vxlan_tunnel_add_del");

    ASSERT_EQ(1u, tunnels.size());
    EXPECT_EQ(1000u, tunnels[0].id);
    EXPECT_FALSE(tunnels[0].flag);
    EXPECT_EQ(1u, vppCallsTo("delete_bvi_interface").size());
}

TEST_F(SwitchVppTunnelNexthop, AFailedRedirectLeavesNoHalfBuiltHostPath)
{
    ASSERT_EQ(SAI_STATUS_SUCCESS, createVlanRif(m_vrB, 3000));

    m_kernelMasters["Vlan3000"] = "Vrft";
    m_redirectStatus = SAI_STATUS_FAILURE;

    g_vppCalls.clear();

    sai_object_id_t nh;

    // the host path is best effort: the tunnel still comes up without one
    ASSERT_EQ(SAI_STATUS_SUCCESS, createNexthop(2000, nh, REMOTE_ROUTER_MAC));

    auto bvi = decapBvi();
    auto pairs = vppCallsTo("configure_lcp_interface");

    // paired, then unpaired again, so no tap is left punting outside the VRF
    ASSERT_EQ(2u, pairs.size());
    EXPECT_EQ(bvi + " tap_" + bvi, pairs[0].name);
    EXPECT_TRUE(pairs[0].flag);
    EXPECT_EQ(bvi + " tap_" + bvi, pairs[1].name);
    EXPECT_FALSE(pairs[1].flag);

    // and removing the next hop has no host path left to tear down
    g_vppCalls.clear();

    ASSERT_EQ(SAI_STATUS_SUCCESS, m_sw->remove(SAI_OBJECT_TYPE_NEXT_HOP, sai_serialize_object_id(nh)));

    EXPECT_TRUE(vppCallsTo("configure_lcp_interface").empty());
}

TEST_F(SwitchVppTunnelNexthop, RemoveTearsDownTheHostPath)
{
    ASSERT_EQ(SAI_STATUS_SUCCESS, createVlanRif(m_vrB, 3000));

    m_kernelMasters["Vlan3000"] = "Vrft";

    g_vppCalls.clear();

    sai_object_id_t nh;

    ASSERT_EQ(SAI_STATUS_SUCCESS, createNexthop(2000, nh, REMOTE_ROUTER_MAC));

    auto bvi = decapBvi();

    g_vppCalls.clear();

    ASSERT_EQ(SAI_STATUS_SUCCESS, m_sw->remove(SAI_OBJECT_TYPE_NEXT_HOP, sai_serialize_object_id(nh)));

    int unpair = vppCallIndex("configure_lcp_interface", bvi + " tap_" + bvi);
    int del = vppCallIndex("delete_bvi_interface", bvi);

    ASSERT_NE(-1, unpair);
    ASSERT_NE(-1, del);
    EXPECT_FALSE(g_vppCalls[unpair].flag);
    EXPECT_LT(unpair, del);
}

/*
 * Routes whose next hop is a host on a VLAN router interface (an SVI, realized
 * in VPP as a BVI). The path must name the BVI as its egress interface, or VPP
 * resolves the next hop recursively in table 0 and drops the traffic.
 */
class SwitchVppVlanRifRoute : public SwitchVppVrf
{
    protected:

        static sai_ip_address_t ipv4(
                const char *addr)
        {
            SWSS_LOG_ENTER();

            sai_ip_address_t ip;

            memset(&ip, 0, sizeof(ip));
            ip.addr_family = SAI_IP_ADDR_FAMILY_IPV4;

            EXPECT_EQ(1, inet_pton(AF_INET, addr, &ip.addr.ip4));

            return ip;
        }

        sai_object_id_t createIpNexthop(
                const char *addr,
                sai_object_id_t rif)
        {
            SWSS_LOG_ENTER();

            auto nh = m_mgr->allocateNewObjectId(SAI_OBJECT_TYPE_NEXT_HOP, m_switchId);

            sai_attribute_t attrs[3];

            attrs[0].id = SAI_NEXT_HOP_ATTR_TYPE;
            attrs[0].value.s32 = SAI_NEXT_HOP_TYPE_IP;
            attrs[1].id = SAI_NEXT_HOP_ATTR_IP;
            attrs[1].value.ipaddr = ipv4(addr);
            attrs[2].id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
            attrs[2].value.oid = rif;

            EXPECT_EQ(SAI_STATUS_SUCCESS,
                    m_sw->create_internal(SAI_OBJECT_TYPE_NEXT_HOP, sai_serialize_object_id(nh), m_switchId, 3, attrs));

            return nh;
        }

        sai_object_id_t createEcmpGroup(
                const std::vector<sai_object_id_t>& nexthops)
        {
            SWSS_LOG_ENTER();

            auto nhg = m_mgr->allocateNewObjectId(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, m_switchId);

            sai_attribute_t gattr;

            gattr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
            gattr.value.s32 = SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_UNORDERED_ECMP;

            EXPECT_EQ(SAI_STATUS_SUCCESS,
                    m_sw->create_internal(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, sai_serialize_object_id(nhg), m_switchId, 1, &gattr));

            for (auto nh: nexthops)
            {
                auto member = m_mgr->allocateNewObjectId(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER, m_switchId);

                sai_attribute_t mattrs[2];

                mattrs[0].id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
                mattrs[0].value.oid = nhg;
                mattrs[1].id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
                mattrs[1].value.oid = nh;

                EXPECT_EQ(SAI_STATUS_SUCCESS,
                        m_sw->create_internal(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER, sai_serialize_object_id(member), m_switchId, 2, mattrs));
            }

            return nhg;
        }

        std::string routeId(
                sai_object_id_t vr,
                const char *prefix)
        {
            SWSS_LOG_ENTER();

            sai_route_entry_t entry;

            memset(&entry, 0, sizeof(entry));
            entry.switch_id = m_switchId;
            entry.vr_id = vr;
            entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;

            EXPECT_EQ(1, inet_pton(AF_INET, prefix, &entry.destination.addr.ip4));
            entry.destination.mask.ip4 = 0xffffffff;

            return sai_serialize_route_entry(entry);
        }

        sai_status_t programRoute(
                sai_object_id_t vr,
                const char *prefix,
                sai_object_id_t nexthop)
        {
            SWSS_LOG_ENTER();

            sai_attribute_t attr;

            attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
            attr.value.oid = nexthop;

            SaiCachedObject route(m_sw.get(), SAI_OBJECT_TYPE_ROUTE_ENTRY, routeId(vr, prefix), 1, &attr);

            uint32_t stats_index = UINT32_MAX;

            return m_sw->IpRouteAddRemove(&route, true, &stats_index);
        }
};

TEST_F(SwitchVppVlanRifRoute, NexthopOnAVlanRouterInterfaceLeavesThroughItsBvi)
{
    auto vr = createVr();
    sai_object_id_t rif;

    ASSERT_EQ(SAI_STATUS_SUCCESS, createVlanRif(vr, 200, &rif));

    auto nh = createIpNexthop("10.20.0.10", rif);

    ASSERT_EQ(SAI_STATUS_SUCCESS, programRoute(vr, "10.250.0.1", nh));

    ASSERT_EQ(1u, g_vppRoutes.size());
    EXPECT_TRUE(g_vppRoutes[0].is_add);
    EXPECT_EQ(tableOf(vr), g_vppRoutes[0].vrf_id);

    ASSERT_EQ(1u, g_vppRoutes[0].paths.size());
    EXPECT_EQ("bvi200", g_vppRoutes[0].paths[0].hwif_name);
}

TEST_F(SwitchVppVlanRifRoute, EcmpMembersOnVlanRouterInterfacesLeaveThroughTheirBvis)
{
    auto vr = createVr();
    sai_object_id_t rif200;
    sai_object_id_t rif300;

    ASSERT_EQ(SAI_STATUS_SUCCESS, createVlanRif(vr, 200, &rif200));
    ASSERT_EQ(SAI_STATUS_SUCCESS, createVlanRif(vr, 300, &rif300));

    auto nhg = createEcmpGroup({
            createIpNexthop("10.20.0.10", rif200),
            createIpNexthop("10.30.0.10", rif300) });

    ASSERT_EQ(SAI_STATUS_SUCCESS, programRoute(vr, "10.250.0.1", nhg));

    ASSERT_EQ(1u, g_vppRoutes.size());
    ASSERT_EQ(2u, g_vppRoutes[0].paths.size());

    std::vector<std::string> hwifs = {
        g_vppRoutes[0].paths[0].hwif_name,
        g_vppRoutes[0].paths[1].hwif_name };

    std::sort(hwifs.begin(), hwifs.end());

    EXPECT_EQ("bvi200", hwifs[0]);
    EXPECT_EQ("bvi300", hwifs[1]);
}

TEST_F(SwitchVppVlanRifRoute, PathAddedOnAVlanRouterInterfaceLeavesThroughItsBvi)
{
    auto vr = createVr();
    sai_object_id_t rif;

    ASSERT_EQ(SAI_STATUS_SUCCESS, createVlanRif(vr, 200, &rif));

    sai_attribute_t attr;

    attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    attr.value.oid = SAI_NULL_OBJECT_ID;

    SaiCachedObject route(m_sw.get(), SAI_OBJECT_TYPE_ROUTE_ENTRY, routeId(vr, "10.250.0.1"), 1, &attr);

    nexthop_grp_member_t member;

    memset(&member, 0, sizeof(member));
    member.addr = ipv4("10.20.0.10");
    member.rif_oid = rif;
    member.weight = 1;
    member.sw_if_index = ~0;

    ASSERT_EQ(SAI_STATUS_SUCCESS, m_sw->IpRoutePathAddRemove(&route, &member, true));

    ASSERT_EQ(1u, g_vppRoutes.size());
    ASSERT_EQ(1u, g_vppRoutes[0].paths.size());
    EXPECT_EQ("bvi200", g_vppRoutes[0].paths[0].hwif_name);
}

/*
 * A LAG in a VLAN: SONiC's PortChannel24 is VPP's BondEthernet24, an untagged
 * member of bridge domain 905, behind a bridge port whose PORT_ID is the LAG.
 * A MAC VPP learns on the bond reaches SAI through the L2 MAC event queue that
 * processFdbEntriesForAging() drains.
 */
class SwitchVppLagFdb : public SwitchVppVrf
{
    protected:

        void SetUp() override
        {
            SwitchVppVrf::SetUp();

            m_vlan = m_mgr->allocateNewObjectId(SAI_OBJECT_TYPE_VLAN, m_switchId);

            sai_attribute_t vattr;

            vattr.id = SAI_VLAN_ATTR_VLAN_ID;
            vattr.value.u16 = VLAN_ID;

            ASSERT_EQ(SAI_STATUS_SUCCESS,
                    m_sw->create_internal(SAI_OBJECT_TYPE_VLAN, sai_serialize_object_id(m_vlan), m_switchId, 1, &vattr));

            m_lag = m_mgr->allocateNewObjectId(SAI_OBJECT_TYPE_LAG, m_switchId);

            sai_attribute_t lattr;

            lattr.id = SAI_LAG_ATTR_PORT_VLAN_ID;
            lattr.value.u16 = VLAN_ID;

            ASSERT_EQ(SAI_STATUS_SUCCESS,
                    m_sw->create_internal(SAI_OBJECT_TYPE_LAG, sai_serialize_object_id(m_lag), m_switchId, 1, &lattr));

            // what vpp_create_lag() and vpp_create_vlan_member() record
            ASSERT_NE(nullptr, m_sw->m_ifaceRegistry.addLag(BOND_ID, BOND_SW_IF_INDEX, m_lag));
            ASSERT_TRUE(m_sw->m_ifaceRegistry.setBdId(BOND_HWIF, VLAN_ID));

            m_bridgePort = m_mgr->allocateNewObjectId(SAI_OBJECT_TYPE_BRIDGE_PORT, m_switchId);

            sai_attribute_t battrs[2];

            battrs[0].id = SAI_BRIDGE_PORT_ATTR_TYPE;
            battrs[0].value.s32 = SAI_BRIDGE_PORT_TYPE_PORT;
            battrs[1].id = SAI_BRIDGE_PORT_ATTR_PORT_ID;
            battrs[1].value.oid = m_lag;

            ASSERT_EQ(SAI_STATUS_SUCCESS,
                    m_sw->create_internal(SAI_OBJECT_TYPE_BRIDGE_PORT, sai_serialize_object_id(m_bridgePort), m_switchId, 2, battrs));

            g_vppCalls.clear();
        }

        // VPP reports a MAC event on the bond, and the FDB aging thread drains it
        void vppMacEvent(
                uint8_t action)
        {
            SWSS_LOG_ENTER();

            vpp_mac_event_t ev;

            memcpy(ev.mac, MAC, sizeof(ev.mac));
            ev.sw_if_index = BOND_SW_IF_INDEX;
            ev.action = action;

            SwitchVpp::staticMacEventCb(&ev, 1, m_sw.get());

            m_sw->processFdbEntriesForAging();
        }

        // the SAI FDB entry of MAC in the VLAN, as ASIC_DB sees it, or null
        std::shared_ptr<SaiAttrWrap> fdbBridgePort()
        {
            SWSS_LOG_ENTER();

            auto& fdbs = m_sw->m_objectHash.at(SAI_OBJECT_TYPE_FDB_ENTRY);

            for (auto& kv: fdbs)
            {
                sai_fdb_entry_t fdb;

                sai_deserialize_fdb_entry(kv.first, fdb);

                if (fdb.bv_id == m_vlan && memcmp(fdb.mac_address, MAC, sizeof(sai_mac_t)) == 0)
                {
                    return kv.second.at("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID");
                }
            }

            return nullptr;
        }

        /*
         * What VirtualSwitchSaiInterface::flushFdbEntries() does around the VPP
         * switch: drop the matching SAI entries and their m_fdb_info_set
         * records (the FLUSHED notification then clears ASIC_DB and STATE_DB),
         * and hand the flush to vpp_fdbentry_flush(). Its result is not
         * checked there, so neither can a failure undo the SAI side.
         */
        sai_status_t flushBridgePort()
        {
            SWSS_LOG_ENTER();

            m_sw->m_objectHash.at(SAI_OBJECT_TYPE_FDB_ENTRY).clear();
            m_sw->m_fdb_info_set.clear();

            sai_attribute_t attrs[2];

            attrs[0].id = SAI_FDB_FLUSH_ATTR_BRIDGE_PORT_ID;
            attrs[0].value.oid = m_bridgePort;
            attrs[1].id = SAI_FDB_FLUSH_ATTR_ENTRY_TYPE;
            attrs[1].value.s32 = SAI_FDB_FLUSH_ENTRY_TYPE_DYNAMIC;

            return m_sw->vpp_fdbentry_flush(m_switchId, 2, attrs);
        }

        std::string fdbSid()
        {
            SWSS_LOG_ENTER();

            sai_fdb_entry_t fdb;

            memset(&fdb, 0, sizeof(fdb));
            fdb.switch_id = m_switchId;
            fdb.bv_id = m_vlan;
            memcpy(fdb.mac_address, MAC, sizeof(sai_mac_t));

            return sai_serialize_fdb_entry(fdb);
        }

        static constexpr uint16_t VLAN_ID = 905;
        static constexpr uint32_t BOND_ID = 24;
        static constexpr uint32_t BOND_SW_IF_INDEX = 124;
        static constexpr const char *BOND_HWIF = "BondEthernet24";
        static constexpr uint8_t MAC[6] = { 0x00, 0x50, 0x56, 0xac, 0xb2, 0x02 };

        sai_object_id_t m_vlan = SAI_NULL_OBJECT_ID;
        sai_object_id_t m_lag = SAI_NULL_OBJECT_ID;
        sai_object_id_t m_bridgePort = SAI_NULL_OBJECT_ID;
};

constexpr uint16_t SwitchVppLagFdb::VLAN_ID;
constexpr uint32_t SwitchVppLagFdb::BOND_ID;
constexpr uint32_t SwitchVppLagFdb::BOND_SW_IF_INDEX;
constexpr const char *SwitchVppLagFdb::BOND_HWIF;
constexpr uint8_t SwitchVppLagFdb::MAC[6];

TEST_F(SwitchVppLagFdb, MacLearnedOnTheBondIsReportedOnTheLagBridgePort)
{
    vppMacEvent(VPP_MAC_ACTION_ADD);

    auto bp = fdbBridgePort();

    ASSERT_NE(nullptr, bp);
    EXPECT_EQ(m_bridgePort, bp->getAttr()->value.oid);
}

/*
 * orchagent flushes a LAG by its bridge port when the LAG goes operationally
 * down, or when a MAC is learned on it while it is still down. The flush has to
 * reach VPP: otherwise the bond keeps the MAC in its L2FIB (bridge domains do
 * not age), VPP never reports it again, and SONiC loses the MAC for good while
 * VPP keeps forwarding to it.
 */
TEST_F(SwitchVppLagFdb, FlushOfTheLagBridgePortFlushesTheBondAndTheMacIsLearnedAgain)
{
    vppMacEvent(VPP_MAC_ACTION_ADD);

    ASSERT_NE(nullptr, fdbBridgePort());

    EXPECT_EQ(SAI_STATUS_SUCCESS, flushBridgePort());

    auto flushes = vppCallsTo("l2fib_flush_int");

    ASSERT_EQ(1u, flushes.size());
    EXPECT_EQ(BOND_HWIF, flushes[0].name);
    EXPECT_TRUE(vppCallsTo("l2fib_flush_all").empty());

    // the next frame from the host: VPP learns the MAC again and reports it
    vppMacEvent(VPP_MAC_ACTION_ADD);

    auto bp = fdbBridgePort();

    ASSERT_NE(nullptr, bp);
    EXPECT_EQ(m_bridgePort, bp->getAttr()->value.oid);
}

/*
 * m_vpp_fdb_entries mirrors what VPP's L2FIB holds. When VPP deletes a MAC the
 * mirror must forget it even if SAI no longer tracks the MAC (a flush, or any
 * other path that dropped it on the SAI side first). A stale mirror entry makes
 * every later ADD for that MAC look like a duplicate, so it is never reported.
 */
TEST_F(SwitchVppLagFdb, MacDeletedByVppAfterSaiDroppedItIsLearnedAgain)
{
    vppMacEvent(VPP_MAC_ACTION_ADD);

    ASSERT_NE(nullptr, fdbBridgePort());

    // SAI drops the entry without VPP being told
    m_sw->m_objectHash.at(SAI_OBJECT_TYPE_FDB_ENTRY).clear();
    m_sw->m_fdb_info_set.clear();

    vppMacEvent(VPP_MAC_ACTION_DELETE);
    vppMacEvent(VPP_MAC_ACTION_ADD);

    auto bp = fdbBridgePort();

    ASSERT_NE(nullptr, bp);
    EXPECT_EQ(m_bridgePort, bp->getAttr()->value.oid);
}

TEST_F(SwitchVppLagFdb, StaticEntryOnTheLagBridgePortIsProgrammedOnTheBond)
{
    sai_attribute_t attrs[2];

    attrs[0].id = SAI_FDB_ENTRY_ATTR_TYPE;
    attrs[0].value.s32 = SAI_FDB_ENTRY_TYPE_STATIC;
    attrs[1].id = SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID;
    attrs[1].value.oid = m_bridgePort;

    ASSERT_EQ(SAI_STATUS_SUCCESS, m_sw->FdbEntryadd(fdbSid(), m_switchId, 2, attrs));

    auto adds = vppCallsTo("l2fib_add_del");

    ASSERT_EQ(1u, adds.size());
    EXPECT_EQ(std::string(BOND_HWIF) + " 00:50:56:ac:b2:02", adds[0].name);
    EXPECT_EQ(VLAN_ID, adds[0].id);
    EXPECT_TRUE(adds[0].flag);      // is_add
    EXPECT_EQ(1u, adds[0].sub_id);  // is_static_mac

    g_vppCalls.clear();

    ASSERT_EQ(SAI_STATUS_SUCCESS, m_sw->FdbEntrydel(fdbSid()));

    auto dels = vppCallsTo("l2fib_add_del");

    ASSERT_EQ(1u, dels.size());
    EXPECT_EQ(std::string(BOND_HWIF) + " 00:50:56:ac:b2:02", dels[0].name);
    EXPECT_EQ(VLAN_ID, dels[0].id);
    EXPECT_FALSE(dels[0].flag);
}

/*
 * orchagent removing a learned MAC on the LAG (e.g. a MAC that moved to a
 * remote VTEP) must clear the mirror, or VPP's next learn of it is dropped as
 * a duplicate.
 */
TEST_F(SwitchVppLagFdb, LearnedEntryRemovedOnTheLagBridgePortIsLearnedAgain)
{
    vppMacEvent(VPP_MAC_ACTION_ADD);

    ASSERT_NE(nullptr, fdbBridgePort());

    ASSERT_EQ(SAI_STATUS_SUCCESS, m_sw->FdbEntrydel(fdbSid()));

    ASSERT_EQ(nullptr, fdbBridgePort());

    vppMacEvent(VPP_MAC_ACTION_ADD);

    EXPECT_NE(nullptr, fdbBridgePort());
}

/*
 * VPP sends its MAC events on the command socket, which saivpp otherwise reads
 * only while a call waits for its reply. A MAC VPP learns while SONiC makes no
 * other VPP call, like a host that sends a frame now and then, or comes back
 * after its PortChannel flapped, must still be reported: the FDB aging pass
 * reads the pending events itself instead of waiting for an unrelated call.
 */
class SwitchVppIdleMacEvents : public SwitchVppLagFdb
{
    protected:

        void SetUp() override
        {
            SwitchVppLagFdb::SetUp();

            g_unreadMacEvents.clear();

            // what Sai::startFdbAgingThread() does
            m_sw->initFdbEventHandling([]() {});

            ASSERT_NE(nullptr, g_macEventCb);
        }

        void TearDown() override
        {
            g_unreadMacEvents.clear();

            SwitchVppLagFdb::TearDown();
        }

        // VPP learns MAC on the bond and sends the event; nothing reads it yet
        void vppSendsMacEvent(
                uint8_t action)
        {
            SWSS_LOG_ENTER();

            vpp_mac_event_t ev;

            memcpy(ev.mac, MAC, sizeof(ev.mac));
            ev.sw_if_index = BOND_SW_IF_INDEX;
            ev.action = action;

            g_unreadMacEvents.push_back(ev);
        }
};

TEST_F(SwitchVppIdleMacEvents, MacLearnedWhileSonicIsIdleIsReportedByTheAgingPass)
{
    vppSendsMacEvent(VPP_MAC_ACTION_ADD);

    // the FDB aging thread's 1 s tick; no other VPP call happens
    m_sw->processFdbEntriesForAging();

    auto bp = fdbBridgePort();

    ASSERT_NE(nullptr, bp);
    EXPECT_EQ(m_bridgePort, bp->getAttr()->value.oid);
    EXPECT_TRUE(g_unreadMacEvents.empty());
}

/*
 * The dev-lab case: PortChannel24 flaps, orchagent flushes its bridge port and
 * saivpp deletes the MACs from the L2FIB. A quiet host's next frame, after
 * orchagent has finished with the flap, makes VPP learn the MAC again.
 */
TEST_F(SwitchVppIdleMacEvents, MacRelearnedAfterAFlushWhileSonicIsIdleIsReported)
{
    vppSendsMacEvent(VPP_MAC_ACTION_ADD);
    m_sw->processFdbEntriesForAging();

    ASSERT_NE(nullptr, fdbBridgePort());

    ASSERT_EQ(SAI_STATUS_SUCCESS, flushBridgePort());
    ASSERT_EQ(nullptr, fdbBridgePort());

    vppSendsMacEvent(VPP_MAC_ACTION_ADD);
    m_sw->processFdbEntriesForAging();

    auto bp = fdbBridgePort();

    ASSERT_NE(nullptr, bp);
    EXPECT_EQ(m_bridgePort, bp->getAttr()->value.oid);
}

/*
 * VPP's L2FIB flushes are lazy: they bump a sequence number and leave the
 * entry until the next scan, and a frame from the MAC before that scan
 * refreshes the entry without a learn event. A MAC with steady traffic, like a
 * node's, then survives every flush and is never reported again. So a flush has
 * to delete each MAC we know VPP holds in its scope, on the interface VPP
 * reported it on, before the lazy flush that catches the rest.
 */
class SwitchVppFdbFlushDeletes : public SwitchVppLagFdb
{
    protected:

        // a second MAC VPP holds on the bond, in another bridge domain
        void trackOtherMac()
        {
            SWSS_LOG_ENTER();

            VppFdbKeyAlias key;

            memcpy(key.mac, OTHER_MAC, sizeof(key.mac));
            key.bd_id = OTHER_BD;

            m_sw->m_vpp_fdb_entries[key] = BOND_SW_IF_INDEX;
        }

        // position of the first call to api in g_vppCalls, or -1
        static int callIndex(
                const std::string& api,
                const std::string& name = "")
        {
            SWSS_LOG_ENTER();

            for (size_t i = 0; i < g_vppCalls.size(); i++)
            {
                if (g_vppCalls[i].api == api && (name.empty() || g_vppCalls[i].name == name))
                {
                    return (int)i;
                }
            }

            return -1;
        }

        sai_status_t flushAttrs(
                std::vector<sai_attribute_t> attrs)
        {
            SWSS_LOG_ENTER();

            return m_sw->vpp_fdbentry_flush(m_switchId, (uint32_t)attrs.size(), attrs.data());
        }

        static sai_attribute_t dynamicOnly()
        {
            SWSS_LOG_ENTER();

            sai_attribute_t attr;

            memset(&attr, 0, sizeof(attr));
            attr.id = SAI_FDB_FLUSH_ATTR_ENTRY_TYPE;
            attr.value.s32 = SAI_FDB_FLUSH_ENTRY_TYPE_DYNAMIC;

            return attr;
        }

        using VppFdbKeyAlias = SwitchVpp::VppFdbKey;

        static constexpr uint32_t OTHER_BD = 906;
        static constexpr uint8_t OTHER_MAC[6] = { 0x00, 0x50, 0x56, 0xac, 0xb2, 0x03 };

        const std::string MAC_ON_BOND = std::string(BOND_HWIF) + " 00:50:56:ac:b2:02";
        const std::string OTHER_MAC_ON_BOND = std::string(BOND_HWIF) + " 00:50:56:ac:b2:03";
};

constexpr uint32_t SwitchVppFdbFlushDeletes::OTHER_BD;
constexpr uint8_t SwitchVppFdbFlushDeletes::OTHER_MAC[6];

TEST_F(SwitchVppFdbFlushDeletes, FlushOfTheLagBridgePortDeletesItsMacBeforeTheLazyFlush)
{
    vppMacEvent(VPP_MAC_ACTION_ADD);

    ASSERT_NE(nullptr, fdbBridgePort());

    // a MAC on an interface that is not this bridge port's stays in VPP
    VppFdbKeyAlias elsewhere;
    memcpy(elsewhere.mac, OTHER_MAC, sizeof(elsewhere.mac));
    elsewhere.bd_id = VLAN_ID;
    m_sw->m_vpp_fdb_entries[elsewhere] = 999;

    g_vppCalls.clear();

    EXPECT_EQ(SAI_STATUS_SUCCESS, flushBridgePort());

    auto dels = vppCallsTo("l2fib_add_del");

    ASSERT_EQ(1u, dels.size());
    EXPECT_EQ(MAC_ON_BOND, dels[0].name);
    EXPECT_EQ(VLAN_ID, dels[0].id);
    EXPECT_FALSE(dels[0].flag);         // is_add
    EXPECT_EQ(0u, dels[0].sub_id);      // is_static_mac

    int del = callIndex("l2fib_add_del", MAC_ON_BOND);
    int flush = callIndex("l2fib_flush_int", BOND_HWIF);

    ASSERT_NE(-1, flush);
    EXPECT_LT(del, flush);

    // forgotten as well, so VPP's next learn of it is reported
    EXPECT_EQ(1u, m_sw->m_vpp_fdb_entries.size());
    EXPECT_EQ(1u, m_sw->m_vpp_fdb_entries.count(elsewhere));
}

TEST_F(SwitchVppFdbFlushDeletes, FlushOfABridgeDomainDeletesOnlyItsMacs)
{
    vppMacEvent(VPP_MAC_ACTION_ADD);

    ASSERT_NE(nullptr, fdbBridgePort());

    trackOtherMac();

    g_vppCalls.clear();

    sai_attribute_t bv;

    memset(&bv, 0, sizeof(bv));
    bv.id = SAI_FDB_FLUSH_ATTR_BV_ID;
    bv.value.u16 = VLAN_ID;     // what vpp_fdbentry_flush() reads the BV_ID as

    EXPECT_EQ(SAI_STATUS_SUCCESS, flushAttrs({bv, dynamicOnly()}));

    auto dels = vppCallsTo("l2fib_add_del");

    ASSERT_EQ(1u, dels.size());
    EXPECT_EQ(MAC_ON_BOND, dels[0].name);
    EXPECT_EQ(VLAN_ID, dels[0].id);
    EXPECT_FALSE(dels[0].flag);

    auto flushes = vppCallsTo("l2fib_flush_bd");

    ASSERT_EQ(1u, flushes.size());
    EXPECT_EQ(VLAN_ID, flushes[0].id);
    EXPECT_LT(callIndex("l2fib_add_del"), callIndex("l2fib_flush_bd"));

    EXPECT_EQ(1u, m_sw->m_vpp_fdb_entries.size());
}

TEST_F(SwitchVppFdbFlushDeletes, FlushAllDeletesEveryTrackedMac)
{
    vppMacEvent(VPP_MAC_ACTION_ADD);

    ASSERT_NE(nullptr, fdbBridgePort());

    trackOtherMac();

    g_vppCalls.clear();

    EXPECT_EQ(SAI_STATUS_SUCCESS, flushAttrs({dynamicOnly()}));

    auto dels = vppCallsTo("l2fib_add_del");

    ASSERT_EQ(2u, dels.size());

    std::vector<std::pair<std::string, uint32_t>> deleted;

    for (auto& d: dels)
    {
        EXPECT_FALSE(d.flag);
        deleted.push_back({d.name, d.id});
    }

    std::sort(deleted.begin(), deleted.end());

    EXPECT_EQ(MAC_ON_BOND, deleted[0].first);
    EXPECT_EQ((uint32_t)VLAN_ID, deleted[0].second);
    EXPECT_EQ(OTHER_MAC_ON_BOND, deleted[1].first);
    EXPECT_EQ(OTHER_BD, deleted[1].second);

    int flush = callIndex("l2fib_flush_all");

    ASSERT_NE(-1, flush);
    EXPECT_LT(callIndex("l2fib_add_del"), flush);

    EXPECT_TRUE(m_sw->m_vpp_fdb_entries.empty());
}

/*
 * EVPN on a VLAN: a P2P VXLAN tunnel to each remote VTEP that advertised the
 * VLAN's VNI, created by orchagent, a tunnel bridge port for each,
 * and the remote MAC addresses as FDB entries on those bridge ports. The
 * fixture's LAG in the same VLAN is the local side.
 */
class SwitchVppEvpnRemoteMac : public SwitchVppLagFdb
{
    protected:

        void SetUp() override
        {
            SwitchVppLagFdb::SetUp();

            if (HasFatalFailure())
            {
                return;
            }

            m_mapper = m_mgr->allocateNewObjectId(SAI_OBJECT_TYPE_TUNNEL_MAP, m_switchId);

            sai_attribute_t mattr;

            mattr.id = SAI_TUNNEL_MAP_ATTR_TYPE;
            mattr.value.s32 = SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID;

            ASSERT_EQ(SAI_STATUS_SUCCESS,
                    m_sw->create_internal(SAI_OBJECT_TYPE_TUNNEL_MAP, sai_serialize_object_id(m_mapper), m_switchId, 1, &mattr));

            g_vppCalls.clear();
            g_vxlanTunnelDst.clear();
        }

        void addMapEntry()
        {
            SWSS_LOG_ENTER();

            m_mapEntry = m_mgr->allocateNewObjectId(SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, m_switchId);

            sai_attribute_t eattrs[4];

            eattrs[0].id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE;
            eattrs[0].value.s32 = SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID;
            eattrs[1].id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP;
            eattrs[1].value.oid = m_mapper;
            eattrs[2].id = SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY;
            eattrs[2].value.u32 = VNI;
            eattrs[3].id = SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE;
            eattrs[3].value.u16 = VLAN_ID;

            ASSERT_EQ(SAI_STATUS_SUCCESS,
                    m_sw->create(SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, sai_serialize_object_id(m_mapEntry), m_switchId, 4, eattrs));
        }

        static sai_ip_address_t ipv4(
                const char *addr)
        {
            SWSS_LOG_ENTER();

            sai_ip_address_t ip;

            memset(&ip, 0, sizeof(ip));
            ip.addr_family = SAI_IP_ADDR_FAMILY_IPV4;

            EXPECT_EQ(1, inet_pton(AF_INET, addr, &ip.addr.ip4));

            return ip;
        }

        // orchagent's P2P tunnel to a remote VTEP, and its tunnel bridge port
        sai_object_id_t addRemoteVtep(
                const char *dst,
                sai_object_id_t *bridgePort = nullptr)
        {
            SWSS_LOG_ENTER();

            auto tunnel = m_mgr->allocateNewObjectId(SAI_OBJECT_TYPE_TUNNEL, m_switchId);

            sai_attribute_t tattrs[4];

            tattrs[0].id = SAI_TUNNEL_ATTR_TYPE;
            tattrs[0].value.s32 = SAI_TUNNEL_TYPE_VXLAN;
            tattrs[1].id = SAI_TUNNEL_ATTR_ENCAP_SRC_IP;
            tattrs[1].value.ipaddr = ipv4(LOCAL_VTEP);
            tattrs[2].id = SAI_TUNNEL_ATTR_ENCAP_DST_IP;
            tattrs[2].value.ipaddr = ipv4(dst);
            tattrs[3].id = SAI_TUNNEL_ATTR_DECAP_MAPPERS;
            tattrs[3].value.objlist.count = 1;
            tattrs[3].value.objlist.list = &m_mapper;

            EXPECT_EQ(SAI_STATUS_SUCCESS,
                    m_sw->create(SAI_OBJECT_TYPE_TUNNEL, sai_serialize_object_id(tunnel), m_switchId, 4, tattrs));

            auto bp = m_mgr->allocateNewObjectId(SAI_OBJECT_TYPE_BRIDGE_PORT, m_switchId);

            sai_attribute_t battrs[2];

            battrs[0].id = SAI_BRIDGE_PORT_ATTR_TYPE;
            battrs[0].value.s32 = SAI_BRIDGE_PORT_TYPE_TUNNEL;
            battrs[1].id = SAI_BRIDGE_PORT_ATTR_TUNNEL_ID;
            battrs[1].value.oid = tunnel;

            EXPECT_EQ(SAI_STATUS_SUCCESS,
                    m_sw->create_internal(SAI_OBJECT_TYPE_BRIDGE_PORT, sai_serialize_object_id(bp), m_switchId, 2, battrs));

            if (bridgePort)
            {
                *bridgePort = bp;
            }

            return tunnel;
        }

        // the sw_if_index of the VXLAN tunnel the stub created to a VTEP, or ~0
        static uint32_t tunnelTo(
                const char *dst)
        {
            SWSS_LOG_ENTER();

            for (auto& kv: g_vxlanTunnelDst)
            {
                if (kv.second == dst)
                {
                    return kv.first;
                }
            }

            return ~0u;
        }

        static std::string onTunnel(
                uint32_t sw_if_index)
        {
            SWSS_LOG_ENTER();

            return std::to_string(sw_if_index) + " " + macStr(REMOTE_MAC);
        }

        std::string remoteSid()
        {
            SWSS_LOG_ENTER();

            sai_fdb_entry_t fdb;

            memset(&fdb, 0, sizeof(fdb));
            fdb.switch_id = m_switchId;
            fdb.bv_id = m_vlan;
            memcpy(fdb.mac_address, REMOTE_MAC, sizeof(sai_mac_t));

            return sai_serialize_fdb_entry(fdb);
        }

        /*
         * What orchagent creates for a type-2 route from a remote VTEP:
         * STATIC, on the VTEP's tunnel bridge port, with the VTEP as the
         * endpoint, and allowed to move unless the route was sticky.
         */
        sai_status_t addRemoteMac(
                sai_object_id_t bridgePort,
                const char *endpoint,
                bool allowMove = true)
        {
            SWSS_LOG_ENTER();

            sai_attribute_t attrs[4];
            uint32_t count = 3;

            attrs[0].id = SAI_FDB_ENTRY_ATTR_TYPE;
            attrs[0].value.s32 = SAI_FDB_ENTRY_TYPE_STATIC;
            attrs[1].id = SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID;
            attrs[1].value.oid = bridgePort;
            attrs[2].id = SAI_FDB_ENTRY_ATTR_ENDPOINT_IP;
            attrs[2].value.ipaddr = ipv4(endpoint);

            if (allowMove)
            {
                attrs[3].id = SAI_FDB_ENTRY_ATTR_ALLOW_MAC_MOVE;
                attrs[3].value.booldata = true;
                count = 4;
            }

            return m_sw->create(SAI_OBJECT_TYPE_FDB_ENTRY, remoteSid(), m_switchId, count, attrs);
        }

        sai_status_t setRemoteMac(
                sai_attr_id_t id,
                sai_object_id_t bridgePort,
                const char *endpoint = nullptr)
        {
            SWSS_LOG_ENTER();

            sai_attribute_t attr;

            memset(&attr, 0, sizeof(attr));
            attr.id = id;

            if (id == SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID)
            {
                attr.value.oid = bridgePort;
            }
            else
            {
                attr.value.ipaddr = ipv4(endpoint);
            }

            return m_sw->set(SAI_OBJECT_TYPE_FDB_ENTRY, remoteSid(), &attr);
        }

        static constexpr uint32_t VNI = 1905;
        static constexpr const char *LOCAL_VTEP = "10.0.0.1";
        static constexpr const char *VTEP_A = "10.0.0.2";
        static constexpr const char *VTEP_B = "10.0.0.3";
        static constexpr const char *VTEP_C = "10.0.0.4";
        static constexpr uint8_t REMOTE_MAC[6] = { 0x00, 0x50, 0x56, 0xac, 0xb0, 0x02 };

        sai_object_id_t m_mapper = SAI_NULL_OBJECT_ID;
        sai_object_id_t m_mapEntry = SAI_NULL_OBJECT_ID;
};

constexpr uint32_t SwitchVppEvpnRemoteMac::VNI;
constexpr const char *SwitchVppEvpnRemoteMac::LOCAL_VTEP;
constexpr const char *SwitchVppEvpnRemoteMac::VTEP_A;
constexpr const char *SwitchVppEvpnRemoteMac::VTEP_B;
constexpr const char *SwitchVppEvpnRemoteMac::VTEP_C;
constexpr uint8_t SwitchVppEvpnRemoteMac::REMOTE_MAC[6];

/*
 * VPP tells VXLAN tunnels apart by source, destination and VNI. Every remote
 * VTEP of a VNI needs its own, or VPP drops what the others send and the
 * switch can send to only one of them.
 */
TEST_F(SwitchVppEvpnRemoteMac, EveryRemoteVtepOfAVniGetsItsOwnTunnel)
{
    addMapEntry();
    addRemoteVtep(VTEP_A);
    addRemoteVtep(VTEP_B);
    addRemoteVtep(VTEP_C);

    auto adds = vppCallsTo("vpp_vxlan_tunnel_add_del");

    ASSERT_EQ(3u, adds.size());

    for (auto& add: adds)
    {
        EXPECT_TRUE(add.flag);
        EXPECT_EQ(VNI, add.id);
    }

    EXPECT_NE(~0u, tunnelTo(VTEP_A));
    EXPECT_NE(~0u, tunnelTo(VTEP_B));
    EXPECT_NE(~0u, tunnelTo(VTEP_C));
}

/*
 * Each VTEP floods BUM traffic to all of its peers itself (head-end
 * replication), so a frame from one tunnel must not go out the others: the
 * tunnels share a split horizon group other than the local ports' 0. MAC
 * addresses behind a tunnel come from EVPN, not from data-plane learning.
 */
TEST_F(SwitchVppEvpnRemoteMac, TunnelsJoinTheVlanInSplitHorizonGroupOneWithLearningOff)
{
    addMapEntry();
    addRemoteVtep(VTEP_A);
    addRemoteVtep(VTEP_B);

    for (auto dst: {VTEP_A, VTEP_B})
    {
        auto swif = std::to_string(tunnelTo(dst));

        int join = vppCallIndex("set_sw_interface_l2_bridge_shg_by_index", swif);

        ASSERT_NE(-1, join) << dst;
        EXPECT_EQ((uint32_t)VLAN_ID, g_vppCalls[join].id);
        EXPECT_EQ(1u, g_vppCalls[join].sub_id) << "split horizon group of the tunnel to " << dst;
        EXPECT_TRUE(g_vppCalls[join].flag);

        int learn = vppCallIndex("set_sw_interface_l2_flags_by_index", swif);

        ASSERT_NE(-1, learn) << dst;
        EXPECT_EQ((uint32_t)VPP_BD_FLAG_LEARN, g_vppCalls[learn].id);
        EXPECT_FALSE(g_vppCalls[learn].flag);
        // joining the bridge domain turns learning on, so it goes off after
        EXPECT_LT(join, learn);
    }
}

TEST_F(SwitchVppEvpnRemoteMac, RemoteMacIsProgrammedOnTheTunnelToItsVtep)
{
    addMapEntry();

    sai_object_id_t bpA, bpB;

    addRemoteVtep(VTEP_A, &bpA);
    addRemoteVtep(VTEP_B, &bpB);

    g_vppCalls.clear();

    ASSERT_EQ(SAI_STATUS_SUCCESS, addRemoteMac(bpB, VTEP_B));

    auto adds = vppCallsTo("l2fib_add_del_by_index");

    ASSERT_EQ(1u, adds.size());
    EXPECT_EQ(onTunnel(tunnelTo(VTEP_B)), adds[0].name);
    EXPECT_EQ((uint32_t)VLAN_ID, adds[0].id);
    EXPECT_TRUE(adds[0].flag);
    // not static: a host that moves here is learned on the local port
    EXPECT_EQ(0u, adds[0].sub_id);

    g_vppCalls.clear();

    ASSERT_EQ(SAI_STATUS_SUCCESS, m_sw->remove(SAI_OBJECT_TYPE_FDB_ENTRY, remoteSid()));

    auto dels = vppCallsTo("l2fib_add_del_by_index");

    ASSERT_EQ(1u, dels.size());
    EXPECT_EQ(onTunnel(tunnelTo(VTEP_B)), dels[0].name);
    EXPECT_EQ((uint32_t)VLAN_ID, dels[0].id);
    EXPECT_FALSE(dels[0].flag);
}

/* a sticky MAC address (no move allowed) stays static in VPP */
TEST_F(SwitchVppEvpnRemoteMac, StickyRemoteMacIsStatic)
{
    addMapEntry();

    sai_object_id_t bpA;

    addRemoteVtep(VTEP_A, &bpA);

    g_vppCalls.clear();

    ASSERT_EQ(SAI_STATUS_SUCCESS, addRemoteMac(bpA, VTEP_A, false));

    auto adds = vppCallsTo("l2fib_add_del_by_index");

    ASSERT_EQ(1u, adds.size());
    EXPECT_EQ(onTunnel(tunnelTo(VTEP_A)), adds[0].name);
    EXPECT_EQ(1u, adds[0].sub_id);
}

/* SAI_FDB_ENTRY_ATTR_ENDPOINT_IP picks the VTEP, not the bridge port's tunnel */
TEST_F(SwitchVppEvpnRemoteMac, EndpointIpPicksTheTunnel)
{
    addMapEntry();

    sai_object_id_t bpA;

    addRemoteVtep(VTEP_A, &bpA);
    addRemoteVtep(VTEP_B);

    g_vppCalls.clear();

    ASSERT_EQ(SAI_STATUS_SUCCESS, addRemoteMac(bpA, VTEP_B));

    auto adds = vppCallsTo("l2fib_add_del_by_index");

    ASSERT_EQ(1u, adds.size());
    EXPECT_EQ(onTunnel(tunnelTo(VTEP_B)), adds[0].name);
}

/*
 * orchagent moves a remote MAC address to another VTEP by setting the bridge
 * port, then the endpoint, one at a time. VPP has to end up with it on the
 * new VTEP's tunnel, and replacing the entry in place leaves no gap in which
 * VPP drops frames to it.
 */
TEST_F(SwitchVppEvpnRemoteMac, RemoteToRemoteMoveRepointsTheMac)
{
    addMapEntry();

    sai_object_id_t bpA, bpB;

    addRemoteVtep(VTEP_A, &bpA);
    addRemoteVtep(VTEP_B, &bpB);

    ASSERT_EQ(SAI_STATUS_SUCCESS, addRemoteMac(bpA, VTEP_A));

    g_vppCalls.clear();

    ASSERT_EQ(SAI_STATUS_SUCCESS, setRemoteMac(SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID, bpB));
    ASSERT_EQ(SAI_STATUS_SUCCESS, setRemoteMac(SAI_FDB_ENTRY_ATTR_ENDPOINT_IP, SAI_NULL_OBJECT_ID, VTEP_B));

    auto calls = vppCallsTo("l2fib_add_del_by_index");

    ASSERT_EQ(1u, calls.size());
    EXPECT_EQ(onTunnel(tunnelTo(VTEP_B)), calls[0].name);
    EXPECT_TRUE(calls[0].flag);

    g_vppCalls.clear();

    ASSERT_EQ(SAI_STATUS_SUCCESS, m_sw->remove(SAI_OBJECT_TYPE_FDB_ENTRY, remoteSid()));

    auto dels = vppCallsTo("l2fib_add_del_by_index");

    ASSERT_EQ(1u, dels.size());
    EXPECT_EQ(onTunnel(tunnelTo(VTEP_B)), dels[0].name);
    EXPECT_FALSE(dels[0].flag);
}

/*
 * A host that moves from a remote VTEP to a local port: VPP moves the entry
 * itself, since it is not static, and reports the move. SONiC has the MAC
 * address already, as a remote one, so that has to reach orchagent as a move
 * to the local bridge port, which it turns into a local MAC.
 */
TEST_F(SwitchVppEvpnRemoteMac, RemoteMacThatMovesToALocalPortIsReportedThere)
{
    addMapEntry();

    sai_object_id_t bpA;

    addRemoteVtep(VTEP_A, &bpA);

    ASSERT_EQ(SAI_STATUS_SUCCESS, addRemoteMac(bpA, VTEP_A));

    vpp_mac_event_t ev;

    memcpy(ev.mac, REMOTE_MAC, sizeof(ev.mac));
    ev.sw_if_index = BOND_SW_IF_INDEX;
    ev.action = VPP_MAC_ACTION_MOVE;

    SwitchVpp::staticMacEventCb(&ev, 1, m_sw.get());

    m_sw->processFdbEntriesForAging();

    auto& entry = m_sw->m_objectHash.at(SAI_OBJECT_TYPE_FDB_ENTRY).at(remoteSid());

    EXPECT_EQ(m_bridgePort, entry.at("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID")->getAttr()->value.oid);
    EXPECT_EQ(SAI_FDB_ENTRY_TYPE_DYNAMIC, entry.at("SAI_FDB_ENTRY_ATTR_TYPE")->getAttr()->value.s32);
}

/*
 * orchagent removes a VTEP's P2P tunnel when the VTEP leaves. Its VPP
 * tunnels go with it, and the tunnels to every other VTEP stay.
 */
TEST_F(SwitchVppEvpnRemoteMac, RemovingTheSaiTunnelRemovesItsL2Tunnels)
{
    addMapEntry();

    sai_object_id_t bpA;

    auto tunnelA = addRemoteVtep(VTEP_A, &bpA);
    addRemoteVtep(VTEP_B);

    ASSERT_EQ(SAI_STATUS_SUCCESS, addRemoteMac(bpA, VTEP_A));

    uint32_t swifA = tunnelTo(VTEP_A);

    g_vppCalls.clear();

    ASSERT_EQ(SAI_STATUS_SUCCESS, m_sw->remove(SAI_OBJECT_TYPE_TUNNEL, sai_serialize_object_id(tunnelA)));

    auto dels = vppCallsTo("vpp_vxlan_tunnel_add_del");

    ASSERT_EQ(1u, dels.size());
    EXPECT_FALSE(dels[0].flag);
    EXPECT_EQ(VNI, dels[0].id);

    int leave = vppCallIndex("set_sw_interface_l2_bridge_shg_by_index", std::to_string(swifA));

    ASSERT_NE(-1, leave);
    EXPECT_FALSE(g_vppCalls[leave].flag);

    // VPP keeps a provisioned L2FIB entry after its interface is gone
    auto macDels = vppCallsTo("l2fib_add_del_by_index");

    ASSERT_EQ(1u, macDels.size());
    EXPECT_EQ(onTunnel(swifA), macDels[0].name);
    EXPECT_FALSE(macDels[0].flag);
}

/*
 * The VNI-to-VLAN map entry can come after the tunnels (and their remote MAC
 * addresses). Every VTEP gets its tunnel then, and the MAC addresses waiting
 * for one are programmed on it.
 */
TEST_F(SwitchVppEvpnRemoteMac, LateMapEntryCreatesEveryTunnelAndProgramsWaitingMacs)
{
    sai_object_id_t bpB;

    addRemoteVtep(VTEP_A);
    addRemoteVtep(VTEP_B, &bpB);

    ASSERT_EQ(SAI_STATUS_SUCCESS, addRemoteMac(bpB, VTEP_B));

    EXPECT_TRUE(vppCallsTo("vpp_vxlan_tunnel_add_del").empty());
    EXPECT_TRUE(vppCallsTo("l2fib_add_del_by_index").empty());

    addMapEntry();

    EXPECT_EQ(2u, vppCallsTo("vpp_vxlan_tunnel_add_del").size());

    auto adds = vppCallsTo("l2fib_add_del_by_index");

    ASSERT_EQ(1u, adds.size());
    EXPECT_EQ(onTunnel(tunnelTo(VTEP_B)), adds[0].name);
    EXPECT_TRUE(adds[0].flag);
}

TEST_F(SwitchVppEvpnRemoteMac, RemovingTheMapEntryRemovesTheTunnelsOfItsVni)
{
    addMapEntry();
    addRemoteVtep(VTEP_A);
    addRemoteVtep(VTEP_B);

    g_vppCalls.clear();

    ASSERT_EQ(SAI_STATUS_SUCCESS, m_sw->remove(SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, sai_serialize_object_id(m_mapEntry)));

    auto dels = vppCallsTo("vpp_vxlan_tunnel_add_del");

    ASSERT_EQ(2u, dels.size());

    for (auto& del: dels)
    {
        EXPECT_FALSE(del.flag);
        EXPECT_EQ(VNI, del.id);
    }
}

/*
 * L2 tunnels do not learn, so a flush of a tunnel bridge port has nothing to
 * take out of VPP. It must not flush the MAC addresses learned on local ports.
 */
TEST_F(SwitchVppEvpnRemoteMac, FlushOfATunnelBridgePortLeavesLocalMacsAlone)
{
    addMapEntry();

    sai_object_id_t bpA;

    addRemoteVtep(VTEP_A, &bpA);

    vppMacEvent(VPP_MAC_ACTION_ADD);

    ASSERT_NE(nullptr, fdbBridgePort());

    g_vppCalls.clear();

    sai_attribute_t attrs[2];

    attrs[0].id = SAI_FDB_FLUSH_ATTR_BRIDGE_PORT_ID;
    attrs[0].value.oid = bpA;
    attrs[1].id = SAI_FDB_FLUSH_ATTR_ENTRY_TYPE;
    attrs[1].value.s32 = SAI_FDB_FLUSH_ENTRY_TYPE_DYNAMIC;

    EXPECT_EQ(SAI_STATUS_SUCCESS, m_sw->vpp_fdbentry_flush(m_switchId, 2, attrs));

    EXPECT_TRUE(vppCallsTo("l2fib_flush_all").empty());
    EXPECT_TRUE(vppCallsTo("l2fib_add_del").empty());
    EXPECT_FALSE(m_sw->m_vpp_fdb_entries.empty());
}

/* a flush that dropped a remote MAC address on the SAI side deletes it from VPP */
TEST_F(SwitchVppEvpnRemoteMac, FlushedRemoteMacIsDeletedFromVpp)
{
    addMapEntry();

    sai_object_id_t bpA;

    addRemoteVtep(VTEP_A, &bpA);

    ASSERT_EQ(SAI_STATUS_SUCCESS, addRemoteMac(bpA, VTEP_A));

    // what VirtualSwitchSaiInterface::flushFdbEntries() does before calling in
    m_sw->m_objectHash.at(SAI_OBJECT_TYPE_FDB_ENTRY).erase(remoteSid());

    g_vppCalls.clear();

    sai_attribute_t attrs[2];

    attrs[0].id = SAI_FDB_FLUSH_ATTR_BRIDGE_PORT_ID;
    attrs[0].value.oid = bpA;
    attrs[1].id = SAI_FDB_FLUSH_ATTR_ENTRY_TYPE;
    attrs[1].value.s32 = SAI_FDB_FLUSH_ENTRY_TYPE_ALL;

    m_sw->vpp_fdbentry_flush(m_switchId, 2, attrs);

    auto dels = vppCallsTo("l2fib_add_del_by_index");

    ASSERT_EQ(1u, dels.size());
    EXPECT_EQ(onTunnel(tunnelTo(VTEP_A)), dels[0].name);
    EXPECT_FALSE(dels[0].flag);
}
