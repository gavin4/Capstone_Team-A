
function run_test() {
  do_x64CFITest("CRASH_X64CFI_PUSH_NONVOL", [
      { symbol: "CRASH_X64CFI_PUSH_NONVOL", trust: "context" },
      { symbol: "CRASH_X64CFI_LAUNCHER", trust: "cfi" },
    ]);
}
