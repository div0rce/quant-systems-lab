(* CLI: replay_snapshot <fixture-file>
   Parses an M15 command-stream fixture, independently replays it, and prints the OCaml-computed
   final snapshot. (Comparing this against the C++ snapshot in the fixture is M17.)
   Exit 0 on success, 2 on parse error. *)

open Qsl_verifier

let opt = function Some p -> string_of_int p | None -> "-"

let print_snapshot (s : Replay_engine.snapshot) =
  Printf.printf "snapshot last_seq %d trades %d\n" s.last_seq s.n_trades;
  List.iter
    (fun (sym : Replay_engine.sym_snapshot) ->
      Printf.printf "sym %d bid %s ask %s orders %d\n" sym.sym (opt sym.best_bid)
        (opt sym.best_ask) sym.order_count;
      List.iter (fun (l : Replay_engine.level_view) -> Printf.printf "level %d B %d %d\n" sym.sym l.price l.qty) sym.bid_levels;
      List.iter (fun (l : Replay_engine.level_view) -> Printf.printf "level %d A %d %d\n" sym.sym l.price l.qty) sym.ask_levels)
    s.symbols

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
      | meta, commands -> print_snapshot (Replay_engine.snapshot (Replay_engine.replay meta commands)))
  | _ ->
      prerr_endline "usage: replay_snapshot <fixture-file>";
      exit 2
