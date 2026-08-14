(* disasm.ml — renders a `.wob` image back to readable mnemonics.

   This is what `woc --dump-bc` prints and what the golden fixtures
   under compiler/test/golden/bc/ pin. Two reasons it decodes the
   *bytes* rather than reading the emitter's in-memory tables:

     - a pinned dump then covers serialization too, so a header offset,
       a pad byte or a table count that goes wrong shows up as a golden
       diff instead of surviving to the loader;
     - the decoder is written against the same normative documents the
       emitter is (docs/plan/oop-vm/00-wob-format.md and
       runtime/src/wob.h), so the two halves disagree loudly.

   Format of the dump (a stable test contract, same doctrine as
   dump.ml's): fixed sections in a fixed order; one line per constant,
   class, interface, vtable row and instruction; a method's line and
   drop tables printed as their own lines before its code, because
   those tables *are* the deliverable for the drop-map and trap-line
   goldens. Registers print as rN, constants kN, classes cN, methods
   mN, interface slots sN, field indexes fN; jumps print their absolute
   target pc, which is what a reader wants and what stays stable when
   an unrelated instruction is inserted before the jump. *)

let magic = 0x31424F57
let hdr_size = 44
let none = 0xFFFFFFFF

exception Bad of string

(* ---- little-endian readers (bounds-checked: a dump must never read
   past a truncated image, however it got truncated) ---- *)

let u8 (s : string) (o : int) : int =
  if o + 1 > String.length s then raise (Bad "truncated");
  String.get_uint8 s o

let u16 (s : string) (o : int) : int =
  if o + 2 > String.length s then raise (Bad "truncated");
  String.get_uint16_le s o

let u32 (s : string) (o : int) : int =
  if o + 4 > String.length s then raise (Bad "truncated");
  Int32.to_int (String.get_int32_le s o) land 0xFFFFFFFF

let i64 (s : string) (o : int) : int64 =
  if o + 8 > String.length s then raise (Bad "truncated");
  String.get_int64_le s o

let op_of i = i land 0xFF
let a_of i = (i lsr 8) land 0xFF
let b_of i = (i lsr 16) land 0xFF
let c_of i = (i lsr 24) land 0xFF
let bx_of i = (i lsr 16) land 0xFFFF
let sbx_of i = bx_of i - 32768

let builtin_name = function
  | 0 -> "now"
  | 1 -> "print"
  | 2 -> "print_int"
  | 3 -> "words"
  | 4 -> "multi_new"
  | 5 -> "multi_push"
  | 6 -> "multi_get"
  | 7 -> "count"
  | 8 -> "latest"
  | 9 -> "map_new"
  | 10 -> "map_set"
  | 11 -> "map_get"
  | 12 -> "map_has"
  | 13 -> "int_to_text"
  | 14 -> "variant_tag"
  | n -> Printf.sprintf "builtin%d" n

let kind_name = function
  | 0 -> "SCALAR"
  | 1 -> "OWNED"
  | 2 -> "GCREF"
  | 3 -> "TEXT"
  | 4 -> "MULTI"
  | 5 -> "MAP"
  | n -> Printf.sprintf "KIND%d" n

(* text constants render with the few escapes a one-line dump needs;
   anything else would let a fixture's newline break the line format *)
let quote (s : string) : string =
  let b = Buffer.create (String.length s + 2) in
  Buffer.add_char b '"';
  String.iter
    (fun ch ->
      match ch with
      | '"' -> Buffer.add_string b "\\\""
      | '\\' -> Buffer.add_string b "\\\\"
      | '\n' -> Buffer.add_string b "\\n"
      | '\t' -> Buffer.add_string b "\\t"
      | c when Char.code c < 32 -> Buffer.add_string b (Printf.sprintf "\\x%02x" (Char.code c))
      | c -> Buffer.add_char b c)
    s;
  Buffer.add_char b '"';
  Buffer.contents b

let mask_str (m : int64) : string =
  if m = 0L then "{}"
  else begin
    let regs = ref [] in
    for r = 63 downto 0 do
      if Int64.logand m (Int64.shift_left 1L r) <> 0L then regs := Printf.sprintf "r%d" r :: !regs
    done;
    "{" ^ String.concat "," !regs ^ "}"
  end

let ins_str (i : int) (pc : int) : string =
  let a = a_of i and b = b_of i and c = c_of i in
  let bx = bx_of i in
  let target = pc + 1 + sbx_of i in
  match op_of i with
  | 0 -> "NOP"
  | 1 -> Printf.sprintf "LOADK     r%d, k%d" a bx
  | 2 -> Printf.sprintf "MOVE      r%d, r%d" a b
  | 3 -> Printf.sprintf "ADD       r%d, r%d, r%d" a b c
  | 4 -> Printf.sprintf "SUB       r%d, r%d, r%d" a b c
  | 5 -> Printf.sprintf "MUL       r%d, r%d, r%d" a b c
  | 6 -> Printf.sprintf "DIV       r%d, r%d, r%d" a b c
  | 7 -> Printf.sprintf "NEG       r%d, r%d" a b
  | 8 -> Printf.sprintf "CONCAT    r%d, r%d, r%d" a b c
  | 9 -> Printf.sprintf "EQ        r%d, r%d, r%d" a b c
  | 10 -> Printf.sprintf "LT        r%d, r%d, r%d" a b c
  | 11 -> Printf.sprintf "LE        r%d, r%d, r%d" a b c
  | 12 -> Printf.sprintf "EQS       r%d, r%d, r%d" a b c
  | 13 -> Printf.sprintf "JMP       -> %04d" target
  | 14 -> Printf.sprintf "JZ        r%d, -> %04d" a target
  | 15 -> Printf.sprintf "CALL      r%d, m%d" a bx
  | 16 -> Printf.sprintf "ICALL     r%d, s%d" a bx
  | 17 -> Printf.sprintf "RET       r%d" a
  | 18 -> "RET0"
  | 19 -> Printf.sprintf "NEW       r%d, c%d" a bx
  | 20 -> Printf.sprintf "GETF      r%d, r%d, f%d" a b c
  | 21 -> Printf.sprintf "SETF      r%d, f%d, r%d" a b c
  | 22 -> Printf.sprintf "DROP      r%d" a
  | 23 -> Printf.sprintf "BORROW_S  r%d" a
  | 24 -> Printf.sprintf "BORROW_X  r%d" a
  | 25 -> Printf.sprintf "RELEASE_S r%d" a
  | 26 -> Printf.sprintf "RELEASE_X r%d" a
  | 27 -> Printf.sprintf "RC_INC    r%d" a
  | 28 -> Printf.sprintf "RC_DEC    r%d" a
  | 29 ->
    if c = 4 || c = 9 then Printf.sprintf "BUILTIN   r%d, kinds=0x%02x, %s" a b (builtin_name c)
    else Printf.sprintf "BUILTIN   r%d, r%d, %s" a b (builtin_name c)
  | 30 -> "DB_STUB"
  | 31 -> Printf.sprintf "TRAP      %d" bx
  (* haxe-parity Task 5: try/catch. The handler target is rendered the way
     jumps are — absolute, so a disassembly can be read against the pc column. *)
  | 32 -> Printf.sprintf "TRY       r%d, handler -> %04d" a target
  | 33 -> "ENDTRY"
  | op -> Printf.sprintf "?OP%d" op

(* ---- the dump ---- *)

type kconst =
  | KInt of int64
  | KText of string

let dump (img : string) : string =
  let out = Buffer.create 4096 in
  let line fmt = Buffer.add_string out (fmt ^ "\n") in
  if u32 img 0 <> magic then raise (Bad "bad magic");
  let ver = u32 img 4 in
  if ver <> 2 then raise (Bad (Printf.sprintf "unsupported version %d" ver));
  let coff = u32 img 8 and ccnt = u32 img 12 in
  let koff = u32 img 16 and kcnt = u32 img 20 in
  let ioff = u32 img 24 and icnt = u32 img 28 in
  let moff = u32 img 32 and mcnt = u32 img 36 in
  let entry = u32 img 40 in
  ignore hdr_size;
  (* constants *)
  let consts = Array.make (max ccnt 1) (KInt 0L) in
  let o = ref coff in
  for i = 0 to ccnt - 1 do
    let tag = u8 img !o in
    incr o;
    if tag = 0 then begin
      consts.(i) <- KInt (i64 img !o);
      o := !o + 8
    end
    else if tag = 1 then begin
      let n = u32 img !o in
      o := !o + 4;
      if !o + n > String.length img then raise (Bad "text constant overruns image");
      consts.(i) <- KText (String.sub img !o n);
      o := !o + n
    end
    else raise (Bad (Printf.sprintf "constant %d: unknown tag %d" i tag))
  done;
  let kname i =
    if i >= ccnt then Printf.sprintf "<k%d?>" i
    else match consts.(i) with KText s -> s | KInt n -> Int64.to_string n
  in
  line "== CONSTANTS ==";
  for i = 0 to ccnt - 1 do
    match consts.(i) with
    | KInt n -> line (Printf.sprintf "k%-3d INT  %Ld" i n)
    | KText s -> line (Printf.sprintf "k%-3d TEXT %s" i (quote s))
  done;
  (* classes *)
  line "== CLASSES ==";
  let o = ref koff in
  for i = 0 to kcnt - 1 do
    let nm = u32 img !o and flags = u32 img (!o + 4) and fcnt = u32 img (!o + 8) in
    o := !o + 12;
    let kinds = List.init fcnt (fun j -> kind_name (u8 img (!o + j))) in
    o := !o + fcnt + ((4 - (fcnt mod 4)) mod 4);
    (* v2 per-field metadata: names, referenced class ids, element kinds. The
       dump shows each field as name:kind — the names are what json.encode
       renders as keys, so a wrong one is worth seeing. *)
    let names = List.init fcnt (fun j -> u32 img (!o + (j * 4))) in
    o := !o + (fcnt * 12);
    let fields =
      List.map2
        (fun nmk k -> if nmk = 0xFFFFFFFF then k else Printf.sprintf "%s:%s" (kname nmk) k)
        names kinds
    in
    line
      (Printf.sprintf "c%-3d %s flags=%s fields=[%s]" i (kname nm)
         (if flags land 1 <> 0 then "gc" else "-")
         (String.concat ", " fields))
  done;
  (* interfaces + vtable rows *)
  line "== INTERFACES ==";
  let o = ref ioff in
  let slot_base = Array.make (max icnt 1) 0 in
  let imcnt = Array.make (max icnt 1) 0 in
  let slots = ref 0 in
  for i = 0 to icnt - 1 do
    let nm = u32 img !o and mc = u32 img (!o + 4) in
    o := !o + 8;
    slot_base.(i) <- !slots;
    imcnt.(i) <- mc;
    line (Printf.sprintf "i%-3d %s methods=%d slots=s%d..s%d" i (kname nm) mc !slots (!slots + mc - 1));
    slots := !slots + mc
  done;
  let vrows = u32 img !o in
  o := !o + 4;
  line "== VTABLES ==";
  for _ = 1 to vrows do
    let cid = u32 img !o and iid = u32 img (!o + 4) in
    o := !o + 8;
    let mc = if iid < icnt then imcnt.(iid) else 0 in
    let ms = List.init mc (fun j -> Printf.sprintf "m%d" (u32 img (!o + (4 * j)))) in
    o := !o + (4 * mc);
    line
      (Printf.sprintf "c%d i%d slots s%d.. -> [%s]" cid iid
         (if iid < icnt then slot_base.(iid) else 0)
         (String.concat ", " ms))
  done;
  (* methods *)
  line "== METHODS ==";
  let o = ref moff in
  for i = 0 to mcnt - 1 do
    let nm = u32 img !o and cid = u32 img (!o + 4) in
    let argc = u8 img (!o + 8) and regc = u8 img (!o + 9) in
    let reserved = u16 img (!o + 10) in
    if reserved <> 0 then raise (Bad "reserved method field is not zero");
    let clen = u32 img (!o + 12) in
    o := !o + 16;
    if clen mod 4 <> 0 then raise (Bad "code length is not a multiple of 4");
    let ninstr = clen / 4 in
    let code = Array.init ninstr (fun j -> u32 img (!o + (4 * j))) in
    o := !o + clen;
    let lcnt = u32 img !o in
    o := !o + 4;
    let lines = List.init lcnt (fun j -> (u32 img (!o + (8 * j)), u32 img (!o + (8 * j) + 4))) in
    o := !o + (8 * lcnt);
    let dcnt = u32 img !o in
    o := !o + 4;
    let drops =
      List.init dcnt (fun j ->
          let base = !o + (20 * j) in
          (u32 img base, i64 img (base + 4), i64 img (base + 12)))
    in
    o := !o + (20 * dcnt);
    line
      (Printf.sprintf "m%-3d %s args=%d regs=%d %s%s" i (kname nm) argc regc
         (if cid = none then "[free fn]" else Printf.sprintf "[class c%d]" cid)
         (if entry = i then " [ENTRY]" else ""));
    line
      (Printf.sprintf "  lines: %s"
         (if lines = [] then "(none)"
          else String.concat " " (List.map (fun (pc, l) -> Printf.sprintf "%d->%d" pc l) lines)));
    if drops = [] then line "  drops: (none)"
    else
      List.iter
        (fun (pc, ow, gc) ->
          line (Printf.sprintf "  drops: pc %d owned=%s gc=%s" pc (mask_str ow) (mask_str gc)))
        drops;
    Array.iteri (fun pc ins -> line (Printf.sprintf "  %04d  %s" pc (ins_str ins pc))) code
  done;
  line "== ENTRY ==";
  line (if entry = none then "(none)" else Printf.sprintf "m%d" entry);
  Buffer.contents out
