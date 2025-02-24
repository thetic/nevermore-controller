#include "fan.hpp"
#include "config.hpp"
#include "handler_helpers.hpp"
#include "nevermore.h"
#include "sdk/ble_data_types.hpp"
#include "sdk/btstack.hpp"
#include "sdk/pwm.hpp"
#include "sensors.hpp"
#include "sensors/tachometer.hpp"
#include "utility/fan_policy.hpp"
#include "utility/timer.hpp"
#include <cstdint>
#include <limits>

using namespace std;

#define FAN_POWER 2B04_01
#define FAN_POWER_OVERRIDE 2B04_02
#define TACHOMETER 03f61fe0_9fe7_4516_98e6_056de551687f_01
// NB: Error prone, but we're the 2nd aggregation char instance in the DB
#define FAN_AGGREGATE 75134bec_dd06_49b1_bac2_c15e05fd7199_02

#define FAN_POLICY_COOLDOWN 2B16_01
#define FAN_POLICY_VOC_PASSIVE_MAX 216aa791_97d0_46ac_8752_60bbc00611e1_03
#define FAN_POLICY_VOC_IMPROVE_MIN 216aa791_97d0_46ac_8752_60bbc00611e1_04

namespace nevermore::gatt::fan {

namespace {

BLE_DECL_SCALAR(RPM16, uint16_t, 1, 0, 0);

constexpr uint8_t FAN_POLICY_UPDATE_RATE_HZ = 10;

constexpr uint8_t TACHOMETER_PULSE_PER_REVOLUTION = 2;
constexpr uint32_t FAN_PWN_HZ = 25'000;

constexpr auto SLICE_PWM = pwm_gpio_to_slice_num_(PIN_FAN_PWM);
constexpr auto SLICE_TACHOMETER = pwm_gpio_to_slice_num_(PIN_FAN_TACHOMETER);
static_assert(pwm_gpio_to_channel_(PIN_FAN_TACHOMETER) == PWM_CHAN_B, "can only read from B channel");

// not included in the fan aggregation - technically a separate service
FanPolicyEnvironmental g_fan_policy;

BLE::Percentage8 g_fan_power = 0;
BLE::Percentage8 g_fan_power_override;  // not-known -> automatic control
nevermore::sensors::Tachometer g_tachometer{PIN_FAN_TACHOMETER, TACHOMETER_PULSE_PER_REVOLUTION};

struct Aggregate {
    BLE::Percentage8 power = g_fan_power;
    BLE::Percentage8 power_override = g_fan_power_override;
    RPM16 tachometer = g_tachometer.revolutions_per_second() * 60;
};

auto g_notify_aggregate = NotifyState<[](hci_con_handle_t conn) {
    att_server_notify(conn, HANDLE_ATTR(FAN_AGGREGATE, VALUE), Aggregate{});
}>();

void fan_power_set(BLE::Percentage8 power) {
    if (g_fan_power == power) return;
    g_fan_power = power;
    g_notify_aggregate.notify();  // `g_fan_power` changed

    auto scale = power.value_or(0) / 100;  // enable automatic control if `NOT_KNOWN`
    auto duty = uint16_t(numeric_limits<uint16_t>::max() * scale);
    pwm_set_gpio_duty(PIN_FAN_PWM, duty);
}

}  // namespace

double fan_power() {
    return g_fan_power.value_or(0);
}

void fan_power_override(BLE::Percentage8 power) {
    if (g_fan_power_override == power) return;

    g_fan_power_override = power;
    g_notify_aggregate.notify();

    if (power != BLE::NOT_KNOWN) {
        fan_power_set(power);  // apply override
    }
}

BLE::Percentage8 fan_power_override() {
    return g_fan_power_override;
}

bool init() {
    // setup PWM configurations for fan PWM and fan tachometer
    auto cfg_pwm = pwm_get_default_config();
    pwm_config_set_freq_hz(cfg_pwm, FAN_PWN_HZ);
    pwm_init(SLICE_PWM, &cfg_pwm, true);

    auto cfg_tachometer = pwm_get_default_config();
    pwm_config_set_clkdiv_mode(&cfg_tachometer, PWM_DIV_B_FALLING);
    pwm_init(SLICE_TACHOMETER, &cfg_tachometer, false);

    // set fan PWM level
    fan_power_set(g_fan_power);

    g_tachometer.start();

    // HACK:  We'd like to notify on write to tachometer changes, but the code base isn't setup
    //        for that yet. Internally poll and update based on diffs for now.
    mk_timer("gatt-fan-tachometer-notify", SENSOR_UPDATE_PERIOD)([](auto*) {
        static double g_prev;
        if (g_prev == g_tachometer.revolutions_per_second()) return;

        g_prev = g_tachometer.revolutions_per_second();
        g_notify_aggregate.notify();
    });

    mk_timer("fan-policy", 1.s / FAN_POLICY_UPDATE_RATE_HZ)([](auto*) {
        static auto g_instance = g_fan_policy.instance();
        if (g_fan_power_override != BLE::NOT_KNOWN) return;

        fan_power_set(g_instance(nevermore::sensors::g_sensors) * 100);
    });

    return true;
}

void disconnected(hci_con_handle_t conn) {
    g_notify_aggregate.unregister(conn);
}

optional<uint16_t> attr_read(
        hci_con_handle_t conn, uint16_t att_handle, uint16_t offset, uint8_t* buffer, uint16_t buffer_size) {
    switch (att_handle) {
        USER_DESCRIBE(FAN_POWER, "Fan %")
        USER_DESCRIBE(FAN_POWER_OVERRIDE, "Fan % - Override")
        USER_DESCRIBE(TACHOMETER, "Fan RPM")
        USER_DESCRIBE(FAN_AGGREGATE, "Aggregated Service Data")

        USER_DESCRIBE(FAN_POLICY_COOLDOWN, "How long to continue filtering after conditions are acceptable")
        USER_DESCRIBE(FAN_POLICY_VOC_PASSIVE_MAX, "Filter if any VOC sensor reaches this threshold")
        USER_DESCRIBE(FAN_POLICY_VOC_IMPROVE_MIN, "Filter if intake exceeds exhaust by this threshold")

        READ_VALUE(FAN_POWER, g_fan_power)
        READ_VALUE(FAN_POWER_OVERRIDE, g_fan_power_override)
        READ_VALUE(TACHOMETER, Aggregate{}.tachometer)
        READ_VALUE(FAN_AGGREGATE, Aggregate{});  // default init populate from global state

        READ_VALUE(FAN_POLICY_COOLDOWN, g_fan_policy.cooldown)
        READ_VALUE(FAN_POLICY_VOC_PASSIVE_MAX, g_fan_policy.voc_passive_max)
        READ_VALUE(FAN_POLICY_VOC_IMPROVE_MIN, g_fan_policy.voc_improve_min)

        READ_CLIENT_CFG(FAN_AGGREGATE, g_notify_aggregate)

    default: return {};
    }
}

optional<int> attr_write(hci_con_handle_t conn, uint16_t att_handle, uint16_t offset, uint8_t const* buffer,
        uint16_t buffer_size) {
    if (buffer_size < offset) return ATT_ERROR_INVALID_OFFSET;
    WriteConsumer consume{offset, buffer, buffer_size};

    switch (att_handle) {
        WRITE_VALUE(FAN_POLICY_COOLDOWN, g_fan_policy.cooldown)
        WRITE_VALUE(FAN_POLICY_VOC_PASSIVE_MAX, g_fan_policy.voc_passive_max)
        WRITE_VALUE(FAN_POLICY_VOC_IMPROVE_MIN, g_fan_policy.voc_improve_min)

        WRITE_CLIENT_CFG(FAN_AGGREGATE, g_notify_aggregate)

    case HANDLE_ATTR(FAN_POWER_OVERRIDE, VALUE): {
        fan_power_override((BLE::Percentage8)consume);
        return 0;
    }

    default: return {};
    }
}

}  // namespace nevermore::gatt::fan
