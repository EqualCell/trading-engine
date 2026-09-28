#!/usr/bin/env python3
"""CLI integration checks for interactive and file replay modes."""

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


def replay_file(folder, name, contents):
    path = folder / name
    path.write_text(contents)
    return run("--replay", path)


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

        missing = run("--replay", folder / "missing.txt")
        check(missing.returncode != 0 and "Cannot open replay file" in missing.stderr and
              "Replay summary:" not in missing.stdout, "missing file should fail clearly")
        directory = run("--replay", folder)
        check(directory.returncode != 0 and "Error reading replay file" in directory.stderr and
              "Replay summary:" not in directory.stdout, "read failure should omit summary")

        for args in [("--replay",), ("--unknown",), ("--replay", "a", "extra")]:
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
