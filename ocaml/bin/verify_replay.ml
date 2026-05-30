(* CLI: verify_replay <fixture-file>
   Parses an exported event-log fixture and checks the replay invariants. Exit 0 if all
   hold, 1 on any violation, 2 on usage error. *)

let () =
  match Sys.argv with
  | [| _; path |] ->
      let fx = Qsl_verifier.Parser.parse_file path in
      let checks = Qsl_verifier.Invariant.run fx in
      List.iter
        (fun (name, ok, detail) ->
          Printf.printf "[%s] %s%s\n" (if ok then "PASS" else "FAIL") name
            (if detail = "" then "" else ": " ^ detail))
        checks;
      if Qsl_verifier.Invariant.all_ok checks then (
        print_endline "OK: all replay invariants hold";
        exit 0)
      else (
        prerr_endline "FAIL: replay invariant violated";
        exit 1)
  | _ ->
      prerr_endline "usage: verify_replay <fixture-file>";
      exit 2
