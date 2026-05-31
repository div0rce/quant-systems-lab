(* CLI: replay_snapshot [--drop-cancels] <fixture-file>
   Parses an M15 command-stream fixture, independently replays it, and prints the OCaml-computed
   final snapshot. (Comparing this against the C++ snapshot in the fixture is M17.)

   `--drop-cancels` is a deliberately buggy oracle that ignores Cancel commands. It is used by the
   divergence demonstration (issue #37) to inject a real C++-vs-OCaml mismatch the shrinker can
   reduce; it has no effect on the normal differential tests.

   Exit 0 on success, 2 on parse error. *)

open Qsl_verifier

let run path drop_cancels =
  match Stream_parser.parse_file path with
  | exception Stream_parser.Parse_error msg ->
      Printf.eprintf "parse error: %s\n" msg;
      exit 2
  | exception Sys_error msg ->
      Printf.eprintf "parse error: %s\n" msg;
      exit 2
  | meta, commands ->
      let commands =
        if drop_cancels then
          List.filter (function Replay_engine.Cancel _ -> false | _ -> true) commands
        else commands
      in
      let s = Replay_engine.snapshot (Replay_engine.replay meta commands) in
      List.iter print_endline (Replay_engine.snapshot_to_lines s)

let () =
  match Sys.argv with
  | [| _; path |] -> run path false
  | [| _; "--drop-cancels"; path |] -> run path true
  | _ ->
      prerr_endline "usage: replay_snapshot [--drop-cancels] <fixture-file>";
      exit 2
