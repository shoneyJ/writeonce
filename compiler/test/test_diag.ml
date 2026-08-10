(* test_diag.ml — minimal unit-test seam for compiler/src/diag.ml.

   This is NOT the golden-file runner (Task 3 adds that as
   compiler/test/runner.ml, alongside this file). Plain assertions,
   printed failures, nonzero exit on any failure — just enough to
   drive diag.ml with TDD before the golden framework exists. *)

let checks = ref 0
let failures = ref 0

let check name cond =
  incr checks;
  if not cond then begin
    incr failures;
    Printf.printf "FAIL: %s\n" name
  end

let check_eq name ~expected ~actual to_string =
  incr checks;
  if expected <> actual then begin
    incr failures;
    Printf.printf "FAIL: %s\n  expected: %s\n  actual:   %s\n" name
      (to_string expected) (to_string actual)
  end

module D = Woc_lib.Diag

(* ---- fixture source text ------------------------------------------ *)

let pricing_wo =
  String.concat "\n"
    [ "type Widget {"
    ; "  field name: Text"
    ; "}"
    ; ""
    ; "fn use_widget() {"
    ; "  let p = Widget { name: \"a\" }"
    ; "  -- noop"
    ; "  -- noop"
    ; "  -- noop"
    ; "  let q = p"
    ; "  -- noop"
    ; "  -- noop"
    ; "  -- noop"
    ; "  take p"
    ]

let lookup file = if file = "pricing.wo" then Some pricing_wo else None

(* ---- rendering shape: code, position, caret excerpt, related site - *)

let () =
  let related =
    [ D.related_site ~file:"pricing.wo" ~line:6 ~col:7 ~label:"moved here" ]
  in
  let d =
    D.error ~code:"WO-E301" ~file:"pricing.wo" ~line:14 ~col:8
      ~message:"value 'p' used after move" ~related ()
  in
  let rendered = D.render lookup d in
  let lines = String.split_on_char '\n' rendered in
  let expected =
    [ "pricing.wo:14:8: error WO-E301: value 'p' used after move"
    ; "  take p"
    ; String.make 7 ' ' ^ "^"
    ; "  pricing.wo:6:7: moved here"
    ; "    let p = Widget { name: \"a\" }"
    ; "  " ^ String.make 6 ' ' ^ "^"
    ]
  in
  check_eq "render: full diagnostic shape (header/excerpt/caret + related)"
    ~expected ~actual:lines (String.concat "\n")

let () =
  (* Warning severity, no related sites: header word changes, no
     trailing related blocks appended. *)
  let d =
    D.warning ~code:"WO-E999" ~file:"pricing.wo" ~line:1 ~col:1
      ~message:"placeholder" ()
  in
  let rendered = D.render lookup d in
  check "render: warning word + no related blocks"
    (rendered = "pricing.wo:1:1: warning WO-E999: placeholder\ntype Widget {\n^")

let () =
  (* Source unavailable: falls back to the header line only, no crash. *)
  let d =
    D.error ~code:"WO-E001" ~file:"missing.wo" ~line:3 ~col:2
      ~message:"no source available" ()
  in
  let rendered = D.render lookup d in
  check "render: missing source falls back to header-only line"
    (rendered = "missing.wo:3:2: error WO-E001: no source available")

let () =
  (* Tabs count as one column: a leading tab is not expanded, so the
     caret offset is exactly (col - 1) characters regardless of what
     the preceding characters are. *)
  let tabby_lookup file = if file = "t.wo" then Some "\tlet p = 1" else None in
  let d = D.error ~code:"WO-E001" ~file:"t.wo" ~line:1 ~col:6 ~message:"x" () in
  let rendered = D.render tabby_lookup d in
  match String.split_on_char '\n' rendered with
  | [ _header; _source; caret ] ->
    check "render: tab counts as one column" (caret = String.make 5 ' ' ^ "^")
  | _ -> check "render: tab counts as one column (unexpected shape)" false

let () =
  (* A defensive edge case, not a real compiler output: line 0 (or
     negative) is out of the 1-based contract. Rendering must fall
     back to a header-only line, matching the missing-file case,
     rather than raising (List.nth_opt raises Invalid_argument on a
     negative index if this guard is ever removed). *)
  let d =
    D.error ~code:"WO-E001" ~file:"pricing.wo" ~line:0 ~col:1
      ~message:"out-of-range line stays non-fatal" ()
  in
  let rendered = D.render lookup d in
  check "render: line < 1 falls back to header-only, does not raise"
    (rendered = "pricing.wo:0:1: error WO-E001: out-of-range line stays non-fatal")

(* ---- collector: source-order accumulation, sort, dedup ------------ *)

let () =
  let c = D.Collector.create () in
  D.Collector.add c
    (D.error ~code:"WO-E100" ~file:"b.wo" ~line:5 ~col:1 ~message:"m1" ());
  D.Collector.add c
    (D.error ~code:"WO-E100" ~file:"a.wo" ~line:10 ~col:2 ~message:"m2" ());
  D.Collector.add c
    (D.error ~code:"WO-E100" ~file:"a.wo" ~line:1 ~col:1 ~message:"m3" ());
  let sites =
    List.map
      (fun (d : D.t) -> (d.site.file, d.site.line, d.site.col))
      (D.Collector.diagnostics c)
  in
  check_eq "collector: output sorted by (file, line, col)"
    ~expected:[ ("a.wo", 1, 1); ("a.wo", 10, 2); ("b.wo", 5, 1) ]
    ~actual:sites (fun l ->
      String.concat ", "
        (List.map (fun (f, ln, cl) -> Printf.sprintf "%s:%d:%d" f ln cl) l))

let () =
  let c = D.Collector.create () in
  D.Collector.add c
    (D.error ~code:"WO-E100" ~file:"a.wo" ~line:1 ~col:1 ~message:"first" ());
  D.Collector.add c
    (D.error ~code:"WO-E100" ~file:"a.wo" ~line:1 ~col:1 ~message:"duplicate" ());
  D.Collector.add c
    (D.error ~code:"WO-E200" ~file:"a.wo" ~line:1 ~col:1
       ~message:"different code, same site" ());
  let ds = D.Collector.diagnostics c in
  check_eq "collector: dedups identical (code, file, line, col)"
    ~expected:2 ~actual:(List.length ds) string_of_int;
  (match ds with
   | [ first; _second ] ->
     check "collector: dedup keeps first-added occurrence"
       (first.message = "first")
   | _ -> check "collector: dedup result shape" false)

(* ---- exit-code decision -------------------------------------------- *)

let () =
  let c = D.Collector.create () in
  check_eq "exit code: empty collector is clean" ~expected:0
    ~actual:(D.Collector.exit_code c) string_of_int

let () =
  let c = D.Collector.create () in
  D.Collector.add c
    (D.warning ~code:"WO-E900" ~file:"a.wo" ~line:1 ~col:1 ~message:"w" ());
  check_eq "exit code: warnings only stays clean" ~expected:0
    ~actual:(D.Collector.exit_code c) string_of_int

let () =
  let c = D.Collector.create () in
  D.Collector.add c
    (D.warning ~code:"WO-E900" ~file:"a.wo" ~line:1 ~col:1 ~message:"w" ());
  D.Collector.add c
    (D.error ~code:"WO-E301" ~file:"a.wo" ~line:2 ~col:1 ~message:"e" ());
  check_eq "exit code: any error reported yields 1" ~expected:1
    ~actual:(D.Collector.exit_code c) string_of_int

let () =
  (* Regression: exit_code must agree with the deduped, displayed
     view, not the raw pre-dedup accumulation. A Warning added first
     and an Error added second at the exact same (code, file, line,
     col) dedup down to just the warning (first occurrence wins) — so
     the report shows zero errors, and exit_code must be 0 to match,
     not 1 from a dropped duplicate's severity. *)
  let c = D.Collector.create () in
  D.Collector.add c
    (D.warning ~code:"WO-E301" ~file:"a.wo" ~line:1 ~col:1
       ~message:"warning first" ());
  D.Collector.add c
    (D.error ~code:"WO-E301" ~file:"a.wo" ~line:1 ~col:1
       ~message:"error second, same code+site" ());
  let ds = D.Collector.diagnostics c in
  check_eq "exit code: dedup survivor decides exit code, not raw items"
    ~expected:1 ~actual:(List.length ds) string_of_int;
  (match ds with
   | [ only ] ->
     check "exit code: dedup keeps the first-added warning"
       (only.severity = D.Warning && only.message = "warning first")
   | _ -> check "exit code: dedup survivor shape" false);
  check_eq "exit code: agrees with the deduped (warning-only) view"
    ~expected:0 ~actual:(D.Collector.exit_code c) string_of_int

(* ---- summary --------------------------------------------------------*)

let () =
  Printf.printf "test_diag: %d checks, %d failures\n" !checks !failures;
  if !failures > 0 then exit 1 else exit 0
