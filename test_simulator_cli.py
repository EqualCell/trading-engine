#!/usr/bin/env python3
"""Focused simulator CLI checks; shared file handling is covered by test_replay."""
from pathlib import Path
import tempfile
from test_replay import ROOT, run, check, check_summary


def account(output, trader, cash, inventory):
    check(f"ACCOUNT trader={trader} cash_paise={cash} inventory={inventory}\n" in output,
          f"wrong account balance: {trader}")


def example(result, timed=False):
    check(result.returncode == 0 and not result.stderr, "example failed")
    check("TRADE buy=102 sell=101 price=10000 qty=3\n" in result.stdout,
          "example trade wrong")
    account(result.stdout, 1, 970000, 3)
    account(result.stdout, 2, 30000, 7)
    check("10000: [id=101, qty=1]" in result.stdout, "remainder missing")
    check("CANCELLED order 101\n" in result.stdout and
          "Best bid: N/A\nBest ask: N/A\nSpread: N/A\n" in result.stdout,
          "final cancellation/quote wrong")
    check_summary(result.stdout, 2, 0, 1, 3, 1)
    check("> " not in result.stdout and "Commands:" not in result.stdout,
          "replay printed interactive help/prompts")
    if timed:
        check(result.stdout.count("Time (us): ") == 10 and
              "Final timestamp (us): 5000\n" in result.stdout, "wrong timestamps")


def main():
    example(run("--simulator", "--replay", ROOT / "examples/accounts.txt"))
    example(run("--simulator", "--replay-timed", ROOT / "examples/timed_accounts.txt"), True)
    with tempfile.TemporaryDirectory() as temp:
        folder = Path(temp)
        csv = folder / "trades.csv"
        example(run("--simulator", "--replay-timed", ROOT / "examples/timed_accounts.txt",
                    "--trades-csv", csv), True)
        check(csv.read_text() == "timestamp_us,buy_id,sell_id,price_paise,quantity\n"
              "2000,102,101,10000,3\n", "wrong simulator CSV")

        def replay(lines, timed=False):
            source = folder / "commands.txt"
            source.write_text(lines)
            return run("--simulator", "--replay-timed" if timed else "--replay", source)

        # Every invalid command must reject exactly once and execution must continue.
        invalid = [
            "CREATE_ACCOUNT 1 99 99", "ACCOUNT 9", "BUY 9 99 100 1", "CANCEL 9 10",
            "CREATE_ACCOUNT 0 0 0", "ACCOUNT 0", "BUY 1 0 100 1", "BUY 0 99 100 1",
            "CANCEL 1 0", "BUY 1 99 0 1", "SELL 1 99 100 0",
            "BUY 1 99 1.5 1", "BUY 1 99 100 -1", "BUY 1 99 1000000001 1",
            "BUY 1 99 100 1000000001", "BUY 1 99 9223372036854775808 1",
            "BUY 1 18446744073709551616 100 1", "ACCOUNT 18446744073709551616",
            "CREATE_ACCOUNT", "CREATE_ACCOUNT 3 0", "CREATE_ACCOUNT 3 0 0 extra",
            "ACCOUNT", "ACCOUNT 1 extra", "BUY 1 99 100", "BUY 1 99 100 1 extra",
            "SELL 99 100 1", "CANCEL", "CANCEL 1", "CANCEL 1 10 extra",
            "BOOK extra", "QUOTE extra", "HELP extra", "EXIT extra", "BOGUS",
        ]
        for value in ["+1", "-", "+", "1.5", "1x", "--1",
                      "9223372036854775808", "-9223372036854775809"]:
            invalid.extend([f"CREATE_ACCOUNT 3 {value} 0", f"CREATE_ACCOUNT 3 0 {value}"])
        # Full signed range parses, then fails the simulator's tighter safety bounds.
        for value in ["9223372036854775807", "-9223372036854775808",
                      "1000000000000000001", "-1000000000000000001"]:
            invalid.append(f"CREATE_ACCOUNT 3 {value} 0")
        for value in ["1000000001", "-1000000001"]:
            invalid.append(f"CREATE_ACCOUNT 3 0 {value}")
        rejected = replay("CREATE_ACCOUNT 1 1000 0\n" + "\n".join(invalid) +
                          "\nCREATE_ACCOUNT 3 -100 -2\nCREATE_ACCOUNT 4 0 0\n"
                          "ACCOUNT 1\nACCOUNT 3\nACCOUNT 4\n"
                          "SELL 3 99 100 1\nBUY 1 100 100 1\nACCOUNT 1\nACCOUNT 3\n")
        check(rejected.returncode == 1 and not rejected.stderr, "validation replay failed")
        check(rejected.stdout.count("Line ") == len(invalid), "wrong rejection count")
        check_summary(rejected.stdout, 2, len(invalid), 1, 1, 0)
        account(rejected.stdout, 1, 1000, 0)
        account(rejected.stdout, 3, -100, -2)
        account(rejected.stdout, 4, 0, 0)
        account(rejected.stdout, 1, 900, 1)
        account(rejected.stdout, 3, 0, -3)
        check(rejected.stdout.count("Initial account exceeds simulator limits") == 6,
              "signed extrema did not reach simulator validation")

        ownership = replay(
            "CREATE_ACCOUNT 1 0 0\nCREATE_ACCOUNT 2 0 0\n"
            "SELL 1 10 100 3\nCANCEL 2 10\nBOOK\nSELL 2 10 90 1\n"
            "BUY 2 11 100 2\nACCOUNT 1\nACCOUNT 2\nCANCEL 1 10\n"
            "SELL 2 10 100 1\nSELL 1 11 100 1\nQUOTE\n")
        check_summary(ownership.stdout, 2, 4, 1, 2, 1)
        check(ownership.returncode == 1 and "[id=10, qty=3]" in ownership.stdout,
              "unauthorized cancellation changed book")
        account(ownership.stdout, 1, 200, -2)
        account(ownership.stdout, 2, -200, 2)

        clock = replay(
            "0 CREATE_ACCOUNT 1 0 0\n\n0 CREATE_ACCOUNT 2 0 0\n"
            "10 ACCOUNT 99\n9 ACCOUNT 1\nbad ACCOUNT 1\n"
            "10 SELL 1 1 100 1\n10 BUY 2 2 100 1\n"
            "10 EXIT extra\n10 ACCOUNT 1\n11 EXIT\n12 ACCOUNT 2\n", True)
        check(clock.returncode == 1 and not clock.stderr, "timed rejection behavior")
        check_summary(clock.stdout, 2, 4, 1, 1, 0)
        check("Line 4: Unknown trader" in clock.stdout and
              "Line 5: timestamp goes backward" in clock.stdout and
              "Line 6: invalid timestamp" in clock.stdout and
              "Final timestamp (us): 11\n" in clock.stdout, "clock/line semantics")
        account(clock.stdout, 1, 100, -1)
        check("\nACCOUNT trader=2" not in clock.stdout, "EXIT failed to stop replay")

        ids = replay("CREATE_ACCOUNT 18446744073709551615 0 0\n"
                     "BUY 18446744073709551615 18446744073709551615 1 1\n"
                     "CANCEL 18446744073709551615 18446744073709551615\n")
        check(ids.returncode == 0, "full positive ID range failed")
        check_summary(ids.stdout, 1, 0, 0, 0, 1)

        limits = replay("CREATE_ACCOUNT 1 -1000000000000000000 -1000000000\n"
                        "CREATE_ACCOUNT 2 1000000000000000000 1000000000\n"
                        "ACCOUNT 1\nACCOUNT 2\n")
        check(limits.returncode == 0, "account bounds rejected")
        account(limits.stdout, 1, -10**18, -10**9)
        account(limits.stdout, 2, 10**18, 10**9)

        for args in [
            ("--simulator", "--replay"), ("--simulator", "--replay", ""),
            ("--simulator", "--replay-timed"), ("--simulator", "--replay-timed", ""),
            ("--simulator", "--unknown"), ("--simulator", "extra"),
            ("--simulator", "--simulator"),
            ("--simulator", "--replay", folder / "commands.txt", "--trades-csv", csv),
            ("--simulator", "--replay-timed", folder / "commands.txt", "--trades-csv"),
            ("--simulator", "--replay-timed", folder / "commands.txt", "--trades-csv", ""),
            ("--simulator", "--replay-timed", folder / "commands.txt", "--unknown", csv),
            ("--simulator", "--replay-timed", folder / "commands.txt", "--trades-csv", csv, "extra"),
            ("--replay", folder / "commands.txt", "--simulator"),
        ]:
            result = run(*args)
            check(result.returncode != 0 and "Usage:" in result.stderr, f"accepted args {args}")

    interactive = run("--simulator", input_text=(ROOT / "examples/accounts.txt").read_text())
    check(interactive.returncode == 0 and not interactive.stderr and
          interactive.stdout.count("> ") == 10 and "Replay summary:" not in interactive.stdout,
          "interactive simulator failed")
    account(interactive.stdout, 1, 970000, 3)
    account(interactive.stdout, 2, 30000, 7)
    help_result = run("--simulator", input_text="help\ncreate_account 1 -0 0\naccount 1\nexit\n")
    check(help_result.stdout.count("CREATE_ACCOUNT trader_id") == 2 and
          "Negative balances are allowed" in help_result.stdout and
          "funds are not reserved" in help_result.stdout and
          "BUY trader_id order_id price quantity" in help_result.stdout,
          "simulator help wrong")
    account(help_result.stdout, 1, 0, 0)
    normal = run(input_text="HELP\nCREATE_ACCOUNT 1 0 0\nBUY 1 2 100 1\nEXIT\n")
    check("BUY id price quantity" in normal.stdout and "CREATE_ACCOUNT trader_id" not in normal.stdout
          and "Unknown command" in normal.stdout and "Usage: BUY id price quantity" in normal.stdout,
          "normal syntax/help changed")
    print("PASS: simulator CLI integration checks")


if __name__ == "__main__":
    main()
