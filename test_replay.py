#!/usr/bin/env python3
"""CLI integration checks for interactive, plain, and timed replay modes."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
ENGINE = ROOT / "trading_engine"


def run(*args, input_text=None):
    return subprocess.run(
        [str(ENGINE), *map(str, args)],
        input=input_text, text=True, capture_output=True, check=False,
    )


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def check_summary(output, accepted, rejected, trades, quantity, cancellations):
    expected = {
        "Accepted orders": accepted,
        "Rejected commands": rejected,
        "Trades": trades,
        "Traded quantity": quantity,
        "Successful cancellations": cancellations,
    }
    check(output.count("Replay summary:") == 1, "expected one replay summary")
    for name, value in expected.items():
        check(f"{name}: {value}\n" in output, f"incorrect {name}")


def replay_file(folder, name, contents, mode="--replay"):
    path = folder / name
    path.write_text(contents)
    return run(mode, path)


def main():
    example = run("--replay", ROOT / "examples" / "orders.txt")
    check(example.returncode == 0, "example replay failed")
    check(not example.stderr and not example.stdout.startswith("\nCommands:"),
          "replay should omit startup help")
    check("> " not in example.stdout, "replay should omit prompts")
    check("TRADE buy=3 sell=1 price=10100 qty=4\n"
          "TRADE buy=3 sell=2 price=10100 qty=2\n" in example.stdout,
          "example trades are wrong")
    first_book, final_book = example.stdout.split("BOOK (prices in paise)")[1:]
    check("10100: [id=2, qty=1]" in first_book and "BIDS: highest price first\n" in first_book,
          "first book is wrong")
    check("Best bid: N/A\nBest ask: 10100\nSpread: N/A" in first_book,
          "first quote is wrong")
    check("CANCELLED order 2" in example.stdout, "cancellation did not succeed")
    check("10100:" not in final_book and
          "Best bid: N/A\nBest ask: N/A\nSpread: N/A" in final_book,
          "final book or quote is wrong")
    check_summary(example.stdout, 3, 0, 2, 6, 1)

    timed_example = run("--replay-timed", ROOT / "examples" / "timed_orders.txt")
    check(timed_example.returncode == 0 and not timed_example.stderr,
          "timed example failed")
    check(timed_example.stdout.count("Time (us): ") == 8 and
          timed_example.stdout.count("Time (us): 0\n") == 2 and
          timed_example.stdout.count("Time (us): 5000\n") == 2,
          "example timestamps are wrong")
    check(timed_example.stdout.count("TRADE ") == 2 and
          "TRADE buy=3 sell=1 price=10100 qty=4\n"
          "TRADE buy=3 sell=2 price=10100 qty=2\n" in timed_example.stdout,
          "timed example trades or FIFO order are wrong")
    check("10100: [id=2, qty=1]" in timed_example.stdout and
          "CANCELLED order 2" in timed_example.stdout and
          "Best bid: N/A\nBest ask: N/A\nSpread: N/A" in timed_example.stdout,
          "timed example book is wrong")
    check_summary(timed_example.stdout, 3, 0, 2, 6, 1)
    check("Final timestamp (us): 5000\n" in timed_example.stdout,
          "timed example final time is wrong")

    with tempfile.TemporaryDirectory() as temp:
        folder = Path(temp)
        empty = replay_file(folder, "empty.txt", "")
        check(empty.returncode == 0, "empty replay should succeed")
        check_summary(empty.stdout, 0, 0, 0, 0, 0)

        invalid = replay_file(folder, "invalid.txt",
                              "\n  \nBOGUS\nSELL 1 10100 1\n"
                              "SELL 1 10100 2\nCANCEL 999\nBUY 2 10100 1\n")
        check(invalid.returncode == 1, "rejected commands should set exit status 1")
        check("Line 3: Unknown command" in invalid.stdout and
              "Line 5: Order ID already used" in invalid.stdout and
              "Line 6: no active order with ID 999" in invalid.stdout,
              "errors should use physical line numbers")
        check(invalid.stdout.count("Line ") == 3 and
              "TRADE buy=2 sell=1 price=10100 qty=1" in invalid.stdout,
              "processing should continue after one rejection per invalid command")
        check_summary(invalid.stdout, 2, 3, 1, 1, 0)

        exits = replay_file(folder, "exit.txt",
                            "EXIT extra\nSELL 1 10100 1\nEXIT\nBUY 2 10100 1\n")
        check(exits.returncode == 1 and "Line 1: EXIT takes no arguments" in exits.stdout,
              "malformed EXIT should be rejected")
        check("ACCEPTED order 1" in exits.stdout and "ACCEPTED order 2" not in exits.stdout,
              "valid EXIT should stop before later lines")
        check_summary(exits.stdout, 1, 1, 0, 0, 0)

        lower = replay_file(folder, "lower.txt", "sell 1 10100 1\nbuy 2 10100 1\n")
        check(lower.returncode == 0 and "TRADE buy=2 sell=1 price=10100 qty=1" in lower.stdout,
              "lowercase commands or EOF replay failed")
        check_summary(lower.stdout, 2, 0, 1, 1, 0)

        timed_empty = replay_file(folder, "timed_empty.txt", "",
                                  "--replay-timed")
        check(timed_empty.returncode == 0 and
              "Final timestamp (us): N/A\n" in timed_empty.stdout,
              "empty timed replay should have no admitted time")
        check_summary(timed_empty.stdout, 0, 0, 0, 0, 0)

        boundary = replay_file(folder, "boundary.txt",
                               "0 SELL 1 100 1\n"
                               "18446744073709551615 BUY 2 100 1\n",
                               "--replay-timed")
        check(boundary.returncode == 0 and
              "TRADE buy=2 sell=1 price=100 qty=1" in boundary.stdout and
              "Final timestamp (us): 18446744073709551615\n" in boundary.stdout,
              "zero or uint64 maximum timestamp failed")
        check_summary(boundary.stdout, 2, 0, 1, 1, 0)

        invalid_time = replay_file(
            folder, "invalid_time.txt",
            "10 SELL 1 100 1\n"
            "9 BUY 2 100 1\n"
            "-1 BUY 3 100 1\n"
            "+1 BUY 4 100 1\n"
            "1.5 BUY 5 100 1\n"
            "abc BUY 6 100 1\n"
            "18446744073709551616 BUY 7 100 1\n"
            "BUY 8 100 1\n"
            "100\n"
            "   \n"
            "9 BOOK\n"
            "11 BUY 9 100 1\n",
            "--replay-timed")
        check(invalid_time.returncode == 1 and
              "Line 2: timestamp goes backward" in invalid_time.stdout and
              "Line 9: missing command" in invalid_time.stdout and
              "Line 11: timestamp goes backward" in invalid_time.stdout and
              "Line 7: timestamp is too large" in invalid_time.stdout,
              "timed validation diagnostics are wrong")
        check(all(f"Line {number}: invalid timestamp" in invalid_time.stdout
                  for number in (3, 4, 5, 6, 8)),
              "malformed and missing timestamps should be rejected")
        check("TRADE buy=9 sell=1 price=100 qty=1" in invalid_time.stdout and
              invalid_time.stdout.count("Line ") == 9 and
              invalid_time.stdout.count("Time (us): ") == 2,
              "invalid lines should not execute or advance time")
        check_summary(invalid_time.stdout, 2, 9, 1, 1, 0)
        check("Final timestamp (us): 11\n" in invalid_time.stdout,
              "blank and rejected lines changed final time")

        advancing = replay_file(folder, "advancing.txt",
                                "10 SELL 1 100 1\n"
                                "20 SELL 1 100 1\n"
                                "15 BUY 2 100 1\n"
                                "20 BUY 3 100 1\n",
                                "--replay-timed")
        check(advancing.returncode == 1 and
              "Line 2: Order ID already used" in advancing.stdout and
              "Line 3: timestamp goes backward" in advancing.stdout and
              "TRADE buy=3 sell=1 price=100 qty=1" in advancing.stdout and
              "TRADE buy=2" not in advancing.stdout,
              "rejected command must still advance the clock")
        check(advancing.stdout.count("Time (us): 20\n") == 2 and
              "Final timestamp (us): 20\n" in advancing.stdout,
              "admitted duplicate should set final time")
        check_summary(advancing.stdout, 2, 2, 1, 1, 0)

        timed_exit = replay_file(folder, "timed_exit.txt",
                                 "0 EXIT extra\n1 HELP\n2 EXIT\n"
                                 "3 BUY 1 100 1\n", "--replay-timed")
        check(timed_exit.returncode == 1 and
              "Line 1: EXIT takes no arguments" in timed_exit.stdout and
              "Commands:" in timed_exit.stdout and
              "ACCEPTED order 1" not in timed_exit.stdout and
              "Final timestamp (us): 2\n" in timed_exit.stdout,
              "timed EXIT handling failed")
        check_summary(timed_exit.stdout, 0, 1, 0, 0, 0)

        only_invalid = replay_file(folder, "only_invalid.txt",
                                   "bad QUOTE\n5\n", "--replay-timed")
        check(only_invalid.returncode == 1 and
              "Final timestamp (us): N/A\n" in only_invalid.stdout,
              "invalid lines must not admit a timestamp")
        check_summary(only_invalid.stdout, 0, 2, 0, 0, 0)

        missing = run("--replay", folder / "missing.txt")
        check(missing.returncode != 0 and "Cannot open replay file" in missing.stderr and
              "Replay summary:" not in missing.stdout, "missing file should fail clearly")
        timed_missing = run("--replay-timed", folder / "missing.txt")
        check(timed_missing.returncode != 0 and
              "Cannot open replay file" in timed_missing.stderr and
              "Replay summary:" not in timed_missing.stdout,
              "timed missing file should omit summary")
        timed_directory = run("--replay-timed", folder)
        check(timed_directory.returncode != 0 and
              "Error reading replay file" in timed_directory.stderr and
              "Replay summary:" not in timed_directory.stdout,
              "timed read failure should omit summary")
        directory = run("--replay", folder)
        check(directory.returncode != 0 and "Error reading replay file" in directory.stderr and
              "Replay summary:" not in directory.stdout, "read failure should omit summary")

        for args in [("--replay",), ("--replay-timed",), ("--unknown",),
                     ("--replay", "a", "extra"), ("--replay-timed", "a", "extra")]:
            bad = run(*args)
            check(bad.returncode != 0 and "Usage:" in bad.stderr,
                  f"invalid invocation {args} should show usage")

    interactive = run(input_text="SELL 1 10100 1\nBUY 2 10100 1\nEXIT\n")
    check(interactive.returncode == 0 and "Commands:" in interactive.stdout and
          interactive.stdout.count("> ") >= 3 and
          "TRADE buy=2 sell=1 price=10100 qty=1" in interactive.stdout and
          "Replay summary:" not in interactive.stdout,
          "interactive mode changed")
    print("PASS: replay CLI integration checks")


if __name__ == "__main__":
    main()
