#include <gtest/gtest.h>
#include <memory>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>

#include <xyz/openbmc_project/Time/error.hpp>

#include "config.h"
#include "bmc_epoch.hpp"
#include "mocked_bmc_time_change_listener.hpp"
#include "types.hpp"


using ::testing::_;
using ::testing::Eq;
using ::testing::Pointee;
using ::testing::MatcherCast;
using ::testing::IsNull;

namespace phosphor
{
namespace time
{

namespace // anonymous
{
constexpr auto SYSTEMD_TIME_SERVICE = "org.freedesktop.timedate1";
constexpr auto SYSTEMD_TIME_PATH = "/org/freedesktop/timedate1";
constexpr auto SYSTEMD_TIME_INTERFACE = "org.freedesktop.timedate1";
constexpr auto METHOD_SET_TIME = "SetTime";
}
using ::testing::_;
using namespace std::chrono;
using namespace std::chrono_literals;
using NotAllowed =
    sdbusplus::xyz::openbmc_project::Time::Error::NotAllowed;

class TestBmcEpoch : public testing::Test
{
    public:
        sdbusplus::SdBusMock mockedSdbus;
        sdbusplus::bus::bus bus;
        sd_event* event;
        MockBmcTimeChangeListener listener;
        std::unique_ptr<BmcEpoch> bmcEpoch;

        TestBmcEpoch()
            : bus(sdbusplus::bus::new_default())
        {
            // BmcEpoch requires sd_event to init
            sd_event_default(&event);
            bus.attach_event(event, SD_EVENT_PRIORITY_NORMAL);
            bmcEpoch = std::make_unique<BmcEpoch>(bus, OBJPATH_BMC);
            bmcEpoch->setBmcTimeChangeListener(&listener);
        }

        ~TestBmcEpoch()
        {
            bus.detach_event();
            sd_event_unref(event);
        }

        // Proxies for BmcEpoch's private members and functions
        Mode getTimeMode()
        {
            return bmcEpoch->timeMode;
        }
        Owner getTimeOwner()
        {
            return bmcEpoch->timeOwner;
        }
        void setTimeOwner(Owner owner)
        {
            bmcEpoch->timeOwner = owner;
        }
        void setTimeMode(Mode mode)
        {
            bmcEpoch->timeMode = mode;
        }
        void triggerTimeChange()
        {
            bmcEpoch->onTimeChange(nullptr,
                                   -1,
                                   0,
                                   bmcEpoch.get());
        }
        void expectSetTimeAllowed(uint64_t t)
        {
            EXPECT_CALL(mockedSdbus, sd_bus_message_new_method_call(
                        IsNull(), _,
                        SYSTEMD_TIME_SERVICE,
                        SYSTEMD_TIME_PATH,
                        SYSTEMD_TIME_INTERFACE,
                        METHOD_SET_TIME)).Times(1);
            // The target time to set
            EXPECT_CALL(mockedSdbus,
	                sd_bus_message_append_basic(_, 'x',
	                    MatcherCast<const void*>(MatcherCast<const int64_t*>
	                    (Pointee(Eq(t))))));
            // The parameters pass to sdbus call
            EXPECT_CALL(mockedSdbus,
	                sd_bus_message_append_basic(_, 'b',
	                    MatcherCast<const void*>(MatcherCast<const int*>
	                    (Pointee(Eq(0)))))).Times(2);
        }

};

TEST_F(TestBmcEpoch, empty)
{
    // Default mode/owner is MANUAL/BOTH
    EXPECT_EQ(Mode::Manual, getTimeMode());
    EXPECT_EQ(Owner::Both, getTimeOwner());
}

TEST_F(TestBmcEpoch, getElapsed)
{
    auto t1 = bmcEpoch->elapsed();
    EXPECT_NE(0, t1);
    auto t2 = bmcEpoch->elapsed();
    EXPECT_GE(t2, t1);
}

TEST_F(TestBmcEpoch, setElapsedNotAllowed)
{
    auto epochNow = duration_cast<microseconds>(
        system_clock::now().time_since_epoch()).count();

    // In Host owner, setting time is not allowed
    setTimeMode(Mode::Manual);
    setTimeOwner(Owner::Host);
    EXPECT_THROW(
        bmcEpoch->elapsed(epochNow),
        NotAllowed);
}

TEST_F(TestBmcEpoch, setElapsedOK)
{
    // Hack the bus with mockedSdbus
    bus = sdbusplus::get_mocked_new(&mockedSdbus);

    setTimeMode(Mode::Manual);
    setTimeOwner(Owner::BMC);

    auto now = bmcEpoch->elapsed();
    microseconds diff = 1min;

    expectSetTimeAllowed(now + diff.count());
    bmcEpoch->elapsed(now + diff.count());
}

TEST_F(TestBmcEpoch, onTimeChange)
{
    // On BMC time change, the listner is expected to be notified
    EXPECT_CALL(listener, onBmcTimeChanged(_)).Times(1);
    triggerTimeChange();
}

}
}
