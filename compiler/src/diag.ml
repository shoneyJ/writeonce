(* diag.ml — the diagnostics module for woc.

   Every later stage (lexer, parser, typechecker, ownership pass)
   reports through this one channel. A diagnostic is:

     - a stable code, e.g. "WO-E301"
     - a severity: Error or Warning
     - a primary site: file, 1-based line, 1-based column
     - a human-readable message
     - zero or more *related* sites (file/line/col + a short label),
       used for two-site errors such as the ownership pass's
       "moved here ... used here"

   Rendering follows the rustc/OCaml convention: a single header line
   ("file:line:col: severity CODE: message"), the source line, and a
   caret line with '^' under the reported column. Related sites render
   the same way, indented beneath the primary diagnostic. Column
   counting treats every character as exactly one column — including
   tab — so the renderer never expands tabs: a caret's horizontal
   offset in the rendered text is always (column - 1) characters,
   matching how a lexer counts columns while scanning raw bytes.

   Rendering needs the actual source text to build an excerpt from, but
   this module never touches the filesystem itself: callers supply a
   `source_lookup` (file name -> file contents option). This keeps
   diag.ml usable from unit tests (in-memory fixtures) and from the
   real driver (reads files) alike, and keeps IO failures a driver
   concern, not a diagnostics-rendering concern.

   The Collector accumulates diagnostics as stages produce them —
   parser/typer/owner recovery can add several in one pass, not
   necessarily in source order. For output it sorts by
   (file, line, col), drops exact (code, file, line, col) duplicates
   (first occurrence wins), and decides the process exit code from
   that same deduped, sorted view: 1 if any diagnostic surviving
   dedup is an error, 0 otherwise. exit_code always agrees with what
   diagnostics/render_all would show — it never inspects the raw,
   pre-dedup accumulation, so a duplicate entry that dedup drops can
   never flip the exit code against what was actually reported. Exit
   code 2 (usage/IO failure) is never decided here — that is
   bin/main.ml's call, made before any compiler stage runs.

   Code ranges are reserved per stage now, so the Task-8 catalog
   (docs/plan/oop-vm/01-error-catalog.md) is an enumeration of codes
   already in use, not an archaeology dig:

     WO-E0xx  lexing     (Task 3)
     WO-E1xx  parsing    (Task 4, 5)
     WO-E2xx  types      (Task 6)
     WO-E3xx  ownership  (Task 7)
     WO-E4xx  emitter    (plan 3 Task 1: bytecode/format limits)

   No codes are minted in this module — it only reserves the ranges.
   The prefixes below are the single documented source later stages
   build their concrete codes from (e.g. lexing_prefix ^ "12" ->
   "WO-E012"). *)

let lexing_prefix = "WO-E0"
let parsing_prefix = "WO-E1"
let types_prefix = "WO-E2"
let ownership_prefix = "WO-E3"
let emitter_prefix = "WO-E4"
let warning_prefix = "WO-W"

type severity =
  | Error
  | Warning

(* A single point in a source file. Both line and col are 1-based. *)
type site = {
  file : string;
  line : int;
  col : int;
}

(* A secondary location attached to a diagnostic, with a short label
   describing its role (e.g. "moved here"). *)
type related = {
  site : site;
  label : string;
}

type t = {
  code : string;
  severity : severity;
  site : site;
  message : string;
  related : related list;
}

(* Alias used inside the nested Collector module below so it can refer
   to the diagnostic type without shadowing its own state type `t`. *)
type diagnostic = t

let related_site ~file ~line ~col ~label : related =
  { site = { file; line; col }; label }

let make ~code ~severity ~file ~line ~col ~message ?(related = []) () : t =
  { code; severity; site = { file; line; col }; message; related }

let error ~code ~file ~line ~col ~message ?(related = []) () : t =
  make ~code ~severity:Error ~file ~line ~col ~message ~related ()

let warning ~code ~file ~line ~col ~message ?(related = []) () : t =
  make ~code ~severity:Warning ~file ~line ~col ~message ~related ()

let severity_word = function
  | Error -> "error"
  | Warning -> "warning"

(* Given a file name, return its full source text, or None if
   unavailable (unreadable, unknown, etc). diag.ml never reads the
   filesystem itself — callers own IO. *)
type source_lookup = string -> string option

let line_text (lookup : source_lookup) (site : site) : string option =
  if site.line < 1 then None
  else
    match lookup site.file with
    | None -> None
    | Some contents ->
      (* site.line is 1-based; String.split_on_char indices are 0-based.
         List.nth_opt raises Invalid_argument (rather than returning
         None) for a negative index, so the guard above is load-bearing:
         without it, a diagnostic constructed with an out-of-range line
         (a bug in some future stage) would crash the renderer instead
         of falling back to a header-only line the way an unknown file
         already does. *)
      List.nth_opt (String.split_on_char '\n' contents) (site.line - 1)

let caret_line (col : int) : string =
  (* Every character — tabs included — counts as one column, so the
     caret's offset is simply (col - 1) plain spaces. We do not expand
     tabs or otherwise inspect the source line's characters here. *)
  String.make (max 0 (col - 1)) ' ' ^ "^"

(* Renders one site as [header] or [header; source-line; caret-line]
   depending on whether an excerpt is available. *)
let render_block (lookup : source_lookup) (site : site) (label : string) :
    string list =
  let header = Printf.sprintf "%s:%d:%d: %s" site.file site.line site.col label in
  match line_text lookup site with
  | None -> [ header ]
  | Some text -> [ header; text; caret_line site.col ]

let render (lookup : source_lookup) (d : t) : string =
  let primary_label =
    Printf.sprintf "%s %s: %s" (severity_word d.severity) d.code d.message
  in
  let primary = render_block lookup d.site primary_label in
  let indent line = "  " ^ line in
  let related_blocks =
    List.concat_map
      (fun (r : related) -> List.map indent (render_block lookup r.site r.label))
      d.related
  in
  String.concat "\n" (primary @ related_blocks)

module Collector = struct
  type t = { mutable items : diagnostic list (* reverse insertion order *) }

  let create () : t = { items = [] }

  let add (c : t) (d : diagnostic) : unit = c.items <- d :: c.items

  let site_key (d : diagnostic) = (d.site.file, d.site.line, d.site.col)

  (* Diagnostics in output order: sorted by (file, line, col) — stable,
     so diagnostics tied on site keep their original insertion
     (source/recovery) order — with exact (code, file, line, col)
     duplicates dropped (first occurrence wins). *)
  let diagnostics (c : t) : diagnostic list =
    let in_insertion_order = List.rev c.items in
    let sorted =
      List.stable_sort
        (fun a b -> compare (site_key a) (site_key b))
        in_insertion_order
    in
    let seen = Hashtbl.create 16 in
    List.filter
      (fun d ->
        let key = (d.code, d.site.file, d.site.line, d.site.col) in
        if Hashtbl.mem seen key then false
        else (
          Hashtbl.add seen key ();
          true))
      sorted

  (* Deliberately scans `diagnostics c` (the sorted, deduped view),
     not raw `c.items`: dedup keeps only the first-inserted occurrence
     per (code, file, line, col), so if a Warning and an Error ever
     land at the exact same (code, site), the surviving displayed
     diagnostic is whichever was added first — and exit_code must
     agree with that same survivor, not with severities dedup already
     discarded. Scanning c.items here would let exit_code see a
     dropped Error that the rendered report no longer shows. *)
  let has_error (c : t) : bool =
    List.exists (fun d -> d.severity = Error) (diagnostics c)

  (* woc's exit-code contract is 0 clean / 1 diagnostics reported / 2
     usage-IO failure. This module only ever returns 0 or 1 — exit
     code 2 is decided by the driver (bin/main.ml) before any compiler
     stage runs, never here. *)
  let exit_code (c : t) : int = if has_error c then 1 else 0

  let render_all (c : t) (lookup : source_lookup) : string =
    diagnostics c |> List.map (render lookup) |> String.concat "\n\n"
end
