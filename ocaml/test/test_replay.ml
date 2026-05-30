(* Tests: the exported valid fixture must pass all invariants; the two hand-crafted
   violation fixtures must fail (proving the checker actually catches violations). *)

let check path expect_ok =
  let fx = Qsl_verifier.Parser.parse_file path in
  let checks = Qsl_verifier.Invariant.run fx in
  let ok = Qsl_verifier.Invariant.all_ok checks in
  if ok <> expect_ok then (
    Printf.eprintf "FAIL %s: expected all_ok=%b but got %b\n" path expect_ok ok;
    List.iter
      (fun (n, o, d) -> Printf.eprintf "  [%b] %s %s\n" o n d)
      checks;
    exit 1)

let () =
  check "fixtures/valid.txt" true;
  check "fixtures/bad_canceled_trade.txt" false;
  check "fixtures/bad_rejected_rest.txt" false;
  print_endline "ocaml replay verifier: all tests passed"
