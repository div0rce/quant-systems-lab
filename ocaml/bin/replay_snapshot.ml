(* CLI: replay_snapshot <fixture-file>
   Parses an M15 command-stream fixture, independently replays it, and prints the OCaml-computed
   final snapshot. (Comparing this against the C++ snapshot in the fixture is M17.)
   Exit 0 on success, 2 on parse error. *)

open Qsl_verifier

let () =
  match Sys.argv with
  | [| _; path |] -> (
      match Stream_parser.parse_file path with
      | exception Stream_parser.Parse_error msg ->
          Printf.eprintf "parse error: %s\n" msg;
          exit 2
      | exception Sys_error msg ->
          Printf.eprintf "parse error: %s\n" msg;
          exit 2
      | meta, commands ->
          let s = Replay_engine.snapshot (Replay_engine.replay meta commands) in
          List.iter print_endline (Replay_engine.snapshot_to_lines s))
  | _ ->
      prerr_endline "usage: replay_snapshot <fixture-file>";
      exit 2
