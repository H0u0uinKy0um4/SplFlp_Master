#include <unity.h>

#include <deque>
#include <vector>

#include "EEPROM.h"
#include "LinkAdapter.h"
#include "ModuleRuntime.h"
#include "Rs485Link.h"
#include "native/master_firmware_tests.h"

// ---------------------------------------------------------------------------
// Host clock and bus stubs. ModuleRuntime is tested through its own interface;
// the real Rs485Link free functions are never called by these tests.
// ---------------------------------------------------------------------------

namespace {

unsigned long hostMillis = 0;

struct ScriptedReply {
    String line;
};

}  // namespace

unsigned long millis() { return hostMillis; }
void delay(unsigned long) {}
void yield() {}

void SerialShim::printf(const char*, ...) {}
void SerialShim::print(const String&) {}
void SerialShim::println(const String&) {}

SerialShim Serial;
EepromShim EEPROM;

void rs485Begin() {}
void rs485SendLine(const String&) {}
String rs485ReadLine(bool) { return String(""); }
void rs485Reset() {}
bool rs485RxBusy() { return false; }
void rs485SendFrame(const uint8_t*, size_t) {}
size_t rs485Available() { return 0; }
int rs485ReadByte() { return -1; }

namespace {

class FakeLink final : public LinkAdapter {
  public:
    std::vector<String> sent;
    std::deque<ScriptedReply> replies;

    void sendLine(const String& line) override { sent.push_back(line); }

    String readLine(bool) override {
        if (replies.empty()) return String("");
        const String line = replies.front().line;
        replies.pop_front();
        return line;
    }

    bool rxBusy() override { return !replies.empty(); }

    void script(const String& line) { replies.push_back(ScriptedReply{line}); }
    void clear() {
        sent.clear();
        replies.clear();
    }
};

String drum() {
    return String("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 +");
}

void scriptModuleQuery(FakeLink& link, long pos) {
    link.script(String("m1:0:4096:0:0=0"));
    link.script(drum());
    link.script(String("m1s:") + String(pos) + ":0:1:1:1.2.3");
}

void assertOnlyLine(FakeLink& link, const char* expected) {
    TEST_ASSERT_EQUAL_UINT(1, link.sent.size());
    TEST_ASSERT_EQUAL_STRING(expected, link.sent[0].c_str());
}

void assertQuerySequence(FakeLink& link) {
    TEST_ASSERT_EQUAL_UINT(3, link.sent.size());
    TEST_ASSERT_EQUAL_STRING("m1d", link.sent[0].c_str());
    TEST_ASSERT_EQUAL_STRING("m1l", link.sent[1].c_str());
    TEST_ASSERT_EQUAL_STRING("m1s", link.sent[2].c_str());
}

}  // namespace

void setUp() {
    hostMillis = 0;
}

void test_move_to_flap_wire_bytes() {
    FakeLink link;
    ModuleRuntime runtime(link);
    TEST_ASSERT_TRUE(runtime.moveToFlap(1, 'A'));
    assertOnlyLine(link, "m1-A");
}

void test_move_to_flap_sanitizes_newline() {
    FakeLink link;
    ModuleRuntime runtime(link);
    TEST_ASSERT_TRUE(runtime.moveToFlap(1, '\n'));
    assertOnlyLine(link, "m1- ");
}

void test_move_to_index_wire_bytes() {
    FakeLink link;
    ModuleRuntime runtime(link);
    TEST_ASSERT_TRUE(runtime.moveToIndex(1, 7));
    assertOnlyLine(link, "m1+7");
}

void test_move_home_wire_bytes() {
    FakeLink link;
    ModuleRuntime runtime(link);
    TEST_ASSERT_TRUE(runtime.moveHome(1));
    assertOnlyLine(link, "m1h");
}

void test_move_steps_wire_bytes() {
    FakeLink link;
    ModuleRuntime runtime(link);
    TEST_ASSERT_TRUE(runtime.moveSteps(1, 100));
    assertOnlyLine(link, "m1g100");
}

void test_record_index_wire_bytes() {
    FakeLink link;
    ModuleRuntime runtime(link);
    TEST_ASSERT_TRUE(runtime.recordIndex(1, 3));
    assertOnlyLine(link, "m1r3");
}

void test_apply_home_offset_wire_bytes() {
    FakeLink link;
    ModuleRuntime runtime(link);
    TEST_ASSERT_TRUE(runtime.applyHomeOffset(1, 12));
    assertOnlyLine(link, "m1o12");
}

void test_clear_position_map_wire_bytes() {
    FakeLink link;
    ModuleRuntime runtime(link);
    TEST_ASSERT_TRUE(runtime.clearPositionMap(1));
    assertOnlyLine(link, "m1e*");
}

void test_write_position_wire_bytes() {
    FakeLink link;
    ModuleRuntime runtime(link);
    TEST_ASSERT_TRUE(runtime.writePosition(1, 3, 1234));
    assertOnlyLine(link, "m1w3:1234");
}

void test_write_drum_character_wire_bytes() {
    FakeLink link;
    ModuleRuntime runtime(link);
    TEST_ASSERT_TRUE(runtime.writeDrumCharacter(1, 3, 'A'));
    assertOnlyLine(link, "m1f3:A");
}

void test_bootloader_arm_wire_bytes() {
    FakeLink link;
    ModuleRuntime runtime(link);
    TEST_ASSERT_TRUE(runtime.enterBootloader(1));
    assertOnlyLine(link, "m1b");
}

void test_application_probe_wire_bytes() {
    FakeLink link;
    ModuleRuntime runtime(link);
    TEST_ASSERT_TRUE(runtime.probeApplicationState(1));
    assertOnlyLine(link, "m1s");
}

void test_invalid_id_sends_nothing() {
    FakeLink link;
    ModuleRuntime runtime(link);
    TEST_ASSERT_FALSE(runtime.moveToFlap(0, 'A'));
    TEST_ASSERT_FALSE(runtime.enterBootloader(256));
    TEST_ASSERT_EQUAL_UINT(0, link.sent.size());
}

void test_query_sequence_wire_bytes() {
    FakeLink link;
    scriptModuleQuery(link, 0);
    ModuleRuntime runtime(link);
    TEST_ASSERT_TRUE(runtime.refreshModule(1));
    assertQuerySequence(link);
}

void test_move_window_defers_readback() {
    FakeLink link;
    scriptModuleQuery(link, 0);
    ModuleRuntime runtime(link);
    TEST_ASSERT_TRUE(runtime.refreshModule(1));
    link.clear();

    TEST_ASSERT_TRUE(runtime.moveHome(1));
    TEST_ASSERT_EQUAL_UINT(1, link.sent.size());

    const auto& modules = runtime.getModules();
    TEST_ASSERT_EQUAL_UINT(1, modules.size());
    TEST_ASSERT_TRUE(modules[0].busyUntilMs > 1000);

    link.clear();
    TEST_ASSERT_FALSE(runtime.tick(1100));
    TEST_ASSERT_EQUAL_UINT(0, link.sent.size());

    scriptModuleQuery(link, 0);
    TEST_ASSERT_TRUE(runtime.tick(5400));
    assertQuerySequence(link);
}

void test_effect_schedules_its_own_deferred_refresh() {
    FakeLink link;
    scriptModuleQuery(link, 0);
    ModuleRuntime runtime(link);
    TEST_ASSERT_TRUE(runtime.refreshModule(1));
    link.clear();

    TEST_ASSERT_TRUE(runtime.moveToIndex(1, 7));
    TEST_ASSERT_EQUAL_UINT(1, link.sent.size());

    link.clear();
    scriptModuleQuery(link, 0);
    TEST_ASSERT_TRUE(runtime.tick(6000));
    assertQuerySequence(link);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_move_to_flap_wire_bytes);
    RUN_TEST(test_move_to_flap_sanitizes_newline);
    RUN_TEST(test_move_to_index_wire_bytes);
    RUN_TEST(test_move_home_wire_bytes);
    RUN_TEST(test_move_steps_wire_bytes);
    RUN_TEST(test_record_index_wire_bytes);
    RUN_TEST(test_apply_home_offset_wire_bytes);
    RUN_TEST(test_clear_position_map_wire_bytes);
    RUN_TEST(test_write_position_wire_bytes);
    RUN_TEST(test_write_drum_character_wire_bytes);
    RUN_TEST(test_bootloader_arm_wire_bytes);
    RUN_TEST(test_application_probe_wire_bytes);
    RUN_TEST(test_invalid_id_sends_nothing);
    RUN_TEST(test_query_sequence_wire_bytes);
    RUN_TEST(test_move_window_defers_readback);
    RUN_TEST(test_effect_schedules_its_own_deferred_refresh);
    RUN_TEST(test_command_trace_suppresses_large_base64_payload);
    RUN_TEST(test_master_sha256_known_vector);
    RUN_TEST(test_master_begin_rejects_invalid_inputs_before_writing);
    RUN_TEST(test_master_streams_verifies_and_applies);
    RUN_TEST(test_master_bad_digest_never_commits);
    RUN_TEST(test_master_rejects_image_larger_than_declared);
    RUN_TEST(test_master_apply_refuses_incomplete_image);
    RUN_TEST(test_master_concurrent_begin_is_rejected);
    RUN_TEST(test_master_cancel_discards_collection);
    RUN_TEST(test_master_status_payload_is_always_complete);
    return UNITY_END();
}
