(* Differential failure artifact bundle (issue #40). On a C++/OCaml snapshot divergence for a
   fixture, write a small reviewable bundle — the original fixture, the OCaml-computed snapshot,
   the C++ expected snapshot, and a unified line diff — so CI can upload it for debugging. *)

open Replay_engine

let read_file path =
  let ic = open_in path in
  Fun.protect ~finally:(fun () -> close_in ic) (fun () ->
      really_input_string ic (in_channel_length ic))

let write_file path s =
  let oc = open_out path in
  Fun.protect ~finally:(fun () -> close_out oc) (fun () -> output_string oc s)

(* A simple line-oriented unified diff: differing lines as "- computed" / "+ expected". *)
let unified computed expected =
  let rec go acc = function
    | c :: cs, e :: es -> go (if c = e then acc else Printf.sprintf "- %s\n+ %s" c e :: acc) (cs, es)
    | c :: cs, [] -> go (("- " ^ c) :: acc) (cs, [])
    | [], e :: es -> go (("+ " ^ e) :: acc) ([], es)
    | [], [] -> List.rev acc
  in
  String.concat "\n" (go [] (computed, expected))

(* Replay `path` and compare the OCaml snapshot against the embedded C++ snapshot. If they
   differ, write <out_dir>/<base>.{original,computed,expected,diff} and return true; otherwise
   return false and write nothing. `out_dir` must already exist. *)
let bundle_if_divergent ~out_dir path =
  let meta, commands, expected = Stream_parser.parse_full_file path in
  let computed = snapshot (replay meta commands) in
  let cl = snapshot_to_lines computed and el = snapshot_to_lines expected in
  if cl = el then false
  else begin
    let base = Filename.remove_extension (Filename.basename path) in
    let p suffix = Filename.concat out_dir (base ^ "." ^ suffix) in
    write_file (p "original") (read_file path);
    write_file (p "computed") (String.concat "\n" cl ^ "\n");
    write_file (p "expected") (String.concat "\n" el ^ "\n");
    write_file (p "diff") (unified cl el ^ "\n");
    true
  end
