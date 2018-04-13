#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>
#include <gtest/gtest.h>

#include "host_epoch.hpp"
#include "utils.hpp"
#include "config.h"
#include "types.hpp"

#include <xyz/openbmc_project/Time/error.hpp>

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

using namespace std::chrono;
using namespace std::chrono_literals;
using NotAllowed =
    sdbusplus::xyz::openbmc_project::Time::Error::NotAllowed;

const constexpr microseconds USEC_ZERO{0};

class TestHostEpoch : public testing::Test
{
    public:
        sdbusplus::SdBusMock mockedSdbus;
        sdbusplus::bus::bus bus;
        HostEpoch hostEpoch;

        static constexpr auto FILE_NOT_EXIST = "path/to/file-not-exist";
        static constexpr auto FILE_OFFSET = "saved_host_offset";
        const microseconds delta = 2s;

        TestHostEpoch()
            : bus(sdbusplus::get_mocked_new(&mockedSdbus)),
              hostEpoch(bus, OBJPATH_HOST)
        {
            // Make sure the file does not exist
            std::remove(FILE_NOT_EXIST);
        }
        ~TestHostEpoch()
        {
            // Cleanup test file
            std::remove(FILE_OFFSET);
        }

        // Proxies for HostEpoch's private members and functions
        Mode getTimeMode()
        {
            return hostEpoch.timeMode;
        }
        Owner getTimeOwner()
        {
            return hostEpoch.timeOwner;
        }
        microseconds getOffset()
        {
            return hostEpoch.offset;
        }
        void setOffset(microseconds us)
        {
            hostEpoch.offset = us;
        }
        void setTimeOwner(Owner owner)
        {
            hostEpoch.onOwnerChanged(owner);
        }
        void setTimeMode(Mode mode)
        {
            hostEpoch.onModeChanged(mode);
        }

        void checkSettingTimeNotAllowed()
        {
            // By default offset shall be 0
            EXPECT_EQ(0, getOffset().count());

            // Set time is not allowed, verify exception is thrown
            microseconds diff = 1min;
            EXPECT_THROW(
                hostEpoch.elapsed(hostEpoch.elapsed() + diff.count()),
                NotAllowed);
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

        void checkSetSplitTimeInFuture()
        {
            // Get current time, and set future +1min time
            auto t1 = hostEpoch.elapsed();
            EXPECT_NE(0, t1);
            microseconds diff = 1min;
            auto t2 = t1 + diff.count();
            hostEpoch.elapsed(t2);

            // Verify that the offset shall be positive,
            // and less or equal to diff, and shall be not too less.
            auto offset = getOffset();
            EXPECT_GT(offset, USEC_ZERO);
            EXPECT_LE(offset, diff);
            diff -= delta;
            EXPECT_GE(offset, diff);

            // Now get time shall be around future +1min time
            auto epochNow = duration_cast<microseconds>(
                                system_clock::now().time_since_epoch()).count();
            auto elapsedGot = hostEpoch.elapsed();
            EXPECT_LT(epochNow, elapsedGot);
            auto epochDiff = elapsedGot - epochNow;
            diff = 1min;
            EXPECT_GT(epochDiff, (diff - delta).count());
            EXPECT_LT(epochDiff, (diff + delta).count());
        }

        void checkSetSplitTimeInPast()
        {
            // Get current time, and set past -1min time
            auto t1 = hostEpoch.elapsed();
            EXPECT_NE(0, t1);
            microseconds diff = 1min;
            auto t2 = t1 - diff.count();
            hostEpoch.elapsed(t2);

            // Verify that the offset shall be negative, and the absolute value
            // shall be equal or greater than diff, and shall not be too greater
            auto offset = getOffset();
            EXPECT_LT(offset, USEC_ZERO);
            offset = -offset;
            EXPECT_GE(offset, diff);
            diff += 10s;
            EXPECT_LE(offset, diff);

            // Now get time shall be around past -1min time
            auto epochNow = duration_cast<microseconds>(
                                system_clock::now().time_since_epoch()).count();
            auto elapsedGot = hostEpoch.elapsed();
            EXPECT_LT(elapsedGot, epochNow);
            auto epochDiff = epochNow - elapsedGot;
            diff = 1min;
            EXPECT_GT(epochDiff, (diff - delta).count());
            EXPECT_LT(epochDiff, (diff + delta).count());
        }
};

TEST_F(TestHostEpoch, empty)
{
    // Default mode/owner is MANUAL/BOTH
    EXPECT_EQ(Mode::Manual, getTimeMode());
    EXPECT_EQ(Owner::Both, getTimeOwner());
}

TEST_F(TestHostEpoch, readDataFileNotExist)
{
    // When file does not exist, the default offset shall be 0
    microseconds offset(0);
    auto value = utils::readData<decltype(offset)::rep>(FILE_NOT_EXIST);
    EXPECT_EQ(0, value);
}

TEST_F(TestHostEpoch, writeAndReadData)
{
    // Write offset to file
    microseconds offsetToWrite(1234567);
    utils::writeData<decltype(offsetToWrite)::rep>(
        FILE_OFFSET, offsetToWrite.count());

    // Read it back
    microseconds offsetToRead;
    offsetToRead = microseconds(
                       utils::readData<decltype(offsetToRead)::rep>(FILE_OFFSET));
    EXPECT_EQ(offsetToWrite, offsetToRead);
}

TEST_F(TestHostEpoch, setElapsedInNtpBmc)
{
    // Set time in NTP/BMC is not allowed
    setTimeMode(Mode::NTP);
    setTimeOwner(Owner::BMC);
    checkSettingTimeNotAllowed();
}

TEST_F(TestHostEpoch, setElapsedInNtpHost)
{
    // Set time in NTP/HOST is not allowed
    setTimeMode(Mode::NTP);
    setTimeOwner(Owner::Host);
    checkSettingTimeNotAllowed();
}

TEST_F(TestHostEpoch, setElapsedInNtpSplit)
{
    // Set time in NTP/SPLIT, offset will be set
    setTimeMode(Mode::NTP);
    setTimeOwner(Owner::Split);

    checkSetSplitTimeInFuture();

    // Reset offset
    setOffset(USEC_ZERO);
    checkSetSplitTimeInPast();
}

TEST_F(TestHostEpoch, setElapsedInNtpBoth)
{
    // Set time in NTP/BOTH is not allowed
    setTimeMode(Mode::NTP);
    setTimeOwner(Owner::Both);
    checkSettingTimeNotAllowed();
}

TEST_F(TestHostEpoch, setElapsedInManualBmc)
{
    // Set time in MANUAL/BMC is not allowed
    setTimeMode(Mode::Manual);
    setTimeOwner(Owner::BMC);
    checkSettingTimeNotAllowed();
}

TEST_F(TestHostEpoch, setElapsedInManualHost)
{
    // Set time in MANUAL/HOST, time will be set to BMC
    // However it requies gmock to test this case
    setTimeMode(Mode::Manual);
    setTimeOwner(Owner::Host);

    auto now = hostEpoch.elapsed();
    microseconds diff = 1min;

    expectSetTimeAllowed(now + diff.count());
    hostEpoch.elapsed(now + diff.count());
}

TEST_F(TestHostEpoch, setElapsedInManualSplit)
{
    // Set to SPLIT owner so that offset will be set
    setTimeMode(Mode::Manual);
    setTimeOwner(Owner::Split);

    checkSetSplitTimeInFuture();

    // Reset offset
    setOffset(USEC_ZERO);
    checkSetSplitTimeInPast();
}

TEST_F(TestHostEpoch, setElapsedInManualBoth)
{
    // Set time in MANUAL/BOTH, time will be set to BMC
    // However it requies gmock to test this case
    setTimeMode(Mode::Manual);
    setTimeOwner(Owner::Both);

    auto now = hostEpoch.elapsed();
    microseconds diff = 3min;

    expectSetTimeAllowed(now + diff.count());
    hostEpoch.elapsed(now + diff.count());
}

TEST_F(TestHostEpoch, setElapsedInSplitAndBmcTimeIsChanged)
{
    // Set to SPLIT owner so that offset will be set
    setTimeOwner(Owner::Split);

    // Get current time, and set future +1min time
    auto t1 = hostEpoch.elapsed();
    EXPECT_NE(0, t1);
    microseconds diff = 1min;
    auto t2 = t1 + diff.count();
    hostEpoch.elapsed(t2);

    // Verify that the offset shall be positive,
    // and less or equal to diff, and shall be not too less.
    auto offset = getOffset();
    EXPECT_GT(offset, USEC_ZERO);
    EXPECT_LE(offset, diff);
    diff -= delta;
    EXPECT_GE(offset, diff);

    // Now BMC time is changed to future +1min
    hostEpoch.onBmcTimeChanged(microseconds(t2));

    // Verify that the offset shall be around zero since it's almost
    // the same as BMC time
    offset = getOffset();
    if (offset.count() < 0)
    {
        offset = microseconds(-offset.count());
    }
    EXPECT_LE(offset, delta);
}

TEST_F(TestHostEpoch, clearOffsetOnOwnerChange)
{
    EXPECT_EQ(USEC_ZERO, getOffset());

    setTimeOwner(Owner::Split);
    hostEpoch.onBmcTimeChanged(microseconds(hostEpoch.elapsed()) + 1min);

    // Now offset shall be non zero
    EXPECT_NE(USEC_ZERO, getOffset());

    setTimeOwner(Owner::Both);

    // Now owner is BOTH, the offset shall be cleared
    EXPECT_EQ(USEC_ZERO, getOffset());
}

}
}
