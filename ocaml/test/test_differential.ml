(* M17 differential replay test: independently replay each fixture's command stream in OCaml,
   then compare the OCaml-computed snapshot against the C++ snapshot embedded in the fixture.
   Equality covers per-symbol best bid/ask, level aggregates, order counts, last_seq, and trade
   count (via the canonical line rendering). A mismatch is reported as a readable line diff. *)

open Qsl_verifier

(* Returns the first differing (computed, expected) line pair, if any. *)
let first_diff computed expected =
  let rec go = function
    | c :: cs, e :: es -> if c = e then go (cs, es) else Some (c, e)
    | c :: _, [] -> Some (c, "<none>")
    | [], e :: _ -> Some ("<none>", e)
    | [], [] -> None
  in
  go (computed, expected)

let differential path =
  let meta, commands, expected = Stream_parser.parse_full_file path in
  let computed = Replay_engine.snapshot (Replay_engine.replay meta commands) in
  let cl = Replay_engine.snapshot_to_lines computed in
  let el = Replay_engine.snapshot_to_lines expected in
  (cl = el, first_diff cl el)

let expect_match path =
  match differential path with
  | true, _ -> ()
  | false, Some (c, e) ->
      Printf.eprintf "FAIL %s: snapshot mismatch\n  computed: %s\n  expected: %s\n" path c e;
      exit 1
  | false, None -> Printf.eprintf "FAIL %s: reported mismatch with no diff\n" path; exit 1

let expect_mismatch path =
  match differential path with
  | false, Some _ -> () (* correctly detected, diff available *)
  | _ -> Printf.eprintf "FAIL %s: expected a detected mismatch but snapshots compared equal\n" path; exit 1

let expect_parse_error path =
  try
    let _ = differential path in
    Printf.eprintf "FAIL %s: expected parser rejection but fixture parsed\n" path;
    exit 1
  with Stream_parser.Parse_error _ -> ()

let () =
  expect_match "fixtures/stream_seed7.txt";
  expect_match "fixtures/stream_ioc.txt";
  expect_mismatch "fixtures/stream_bad_snapshot.txt";
  expect_parse_error "fixtures/bad_snapshot_level_symbol.txt";
  print_endline "ocaml differential replay: all tests passed"
