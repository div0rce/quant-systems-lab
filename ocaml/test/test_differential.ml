(* M17/M18 differential replay test: independently replay each fixture's command stream in OCaml
   and compare the OCaml-computed snapshot against the C++ snapshot embedded in the fixture
   (best bid/ask, level aggregates, order counts, last_seq, trade count), reported as a readable
   line diff. M18 adds property fixtures (seeded enriched flows covering valid/invalid/duplicate/
   reused/unknown/IOC/market/cancel/modify/multi-symbol) and a no-crossed-book invariant; the
   negative fixtures prove the comparison detects divergence in distinct snapshot fields. *)

open Qsl_verifier

let first_diff computed expected =
  let rec go = function
    | c :: cs, e :: es -> if c = e then go (cs, es) else Some (c, e)
    | c :: _, [] -> Some (c, "<none>")
    | [], e :: _ -> Some ("<none>", e)
    | [], [] -> None
  in
  go (computed, expected)

let no_crossed (s : Replay_engine.snapshot) =
  List.for_all
    (fun (x : Replay_engine.sym_snapshot) ->
      match (x.best_bid, x.best_ask) with Some b, Some a -> b < a | _ -> true)
    s.symbols

let differential path =
  let meta, commands, expected = Stream_parser.parse_full_file path in
  let computed = Replay_engine.snapshot (Replay_engine.replay meta commands) in
  ( computed,
    first_diff (Replay_engine.snapshot_to_lines computed) (Replay_engine.snapshot_to_lines expected) )

(* C++ and OCaml snapshots must match, and the OCaml book must not be crossed. *)
let expect_match path =
  let computed, diff = differential path in
  (match diff with
  | None -> ()
  | Some (c, e) ->
      Printf.eprintf "FAIL %s: C++/OCaml snapshot mismatch\n  computed: %s\n  expected: %s\n" path c e;
      exit 1);
  if not (no_crossed computed) then (
    Printf.eprintf "FAIL %s: crossed book in OCaml replay\n" path;
    exit 1)

(* The comparison must DETECT a corrupted snapshot (negative coverage). *)
let expect_mismatch path =
  match differential path with
  | _, Some _ -> ()
  | _, None ->
      Printf.eprintf "FAIL %s: expected a detected mismatch but snapshots compared equal\n" path;
      exit 1

(* A structurally malformed fixture (e.g. a level line whose symbol does not match) must be
   rejected by the parser, not silently accepted. *)
let expect_parse_error path =
  try
    let _ = differential path in
    Printf.eprintf "FAIL %s: expected parser rejection but fixture parsed\n" path;
    exit 1
  with Stream_parser.Parse_error _ -> ()

let property_fixtures () =
  Sys.readdir "fixtures" |> Array.to_list
  |> List.filter (fun f ->
         String.length f >= 5 && String.sub f 0 5 = "prop_" && Filename.check_suffix f ".txt")
  |> List.sort compare
  |> List.map (fun f -> "fixtures/" ^ f)

let () =
  expect_match "fixtures/stream_seed7.txt";
  expect_match "fixtures/stream_ioc.txt";
  expect_match "fixtures/shrunk_seed1.txt"; (* M19: minimized counterexample replays independently *)
  List.iter expect_match (property_fixtures ());
  (* negative coverage: each corrupts a distinct snapshot field *)
  expect_mismatch "fixtures/stream_bad_snapshot.txt"; (* ask-level qty *)
  expect_mismatch "fixtures/stream_bad_lastseq.txt"; (* last_seq *)
  expect_mismatch "fixtures/stream_bad_orders.txt"; (* order_count *)
  expect_parse_error "fixtures/bad_snapshot_level_symbol.txt"; (* M17: malformed level ownership *)
  Printf.printf "ocaml differential replay: all tests passed (%d property fixtures)\n"
    (List.length (property_fixtures ()))
