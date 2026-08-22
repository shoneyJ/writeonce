# operators — iteration 36's manual-test workload

Exercises everything iteration 36 added to the language: boolean `not`,
the five Int bitwise operators (`&` `|` `^` `<<` `>>`), hex/binary/
underscore integer literals, and the five compound assigns
(`+=` `-=` `*=` `/=` `%=`).

## Run it

```
woc .            # from this directory
./target/operators
```

Every line prints `ok <label> = <value>` — each label states the exact
expression and the expected result, so a manual test is reading the
lines and spotting any `FAIL`. The sections, in order:

1. **Literals** — `0xFF`, `0b1010_1010`, `1_000_000` against their
   decimal twins.
2. **The five operators** — including `-16 >> 2 = -4` (`>>` is
   ARITHMETIC: the sign bit extends, the story's settled decision 1)
   and complement spelled `-1 ^ x` (no `~` in this language).
3. **Precedence pins** — Go's C-trap fix: `&`/`<<`/`>>` bind with
   `*`, `|`/`^` bind with `+`, all above comparison, so
   `x & mask == 0` groups the AND first and `1 << 4 + 1` is 17, not 32.
4. **`not`** — a word like `and`/`or`, Lua's unary placement:
   `not a == b` groups `(not a) == b`.
5. **Compound assigns** — one value threaded through all five.
6. **The consumer proof** — HMAC's ipad/opad step (story 34's blocker):
   `byte_at(key, i) ^ 0x36` / `^ 0x5C` in pure `.wo`.

## The trap

```
./target/operators trap
```

must die with `trap 12 in shift_by ...: shift count out of range 0..63`
(exit 1). A shift count outside 0..63 is a RUN-TIME trap only when the
count is a variable — a literal out-of-range count never compiles
(WO-E223, try changing `1 << 6` to `1 << 64`).
