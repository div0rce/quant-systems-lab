(* Tests the differential failure artifact bundle: a divergent fixture writes a bundle (with a
   non-empty diff); a matching fixture writes nothing. *)

open Qsl_verifier

let () =
  let out = "bundle_out" in
  (try Sys.mkdir out 0o755 with Sys_error _ -> ());

  (* A divergent fixture (corrupted snapshot) must produce a bundle. *)
  if not (Diff_report.bundle_if_divergent ~out_dir:out "fixtures/stream_bad_snapshot.txt") then (
    prerr_endline "FAIL: expected divergence for stream_bad_snapshot.txt";
    exit 1);
  List.iter
    (fun suffix ->
      let f = Filename.concat out ("stream_bad_snapshot." ^ suffix) in
      if not (Sys.file_exists f) then (
        Printf.eprintf "FAIL: missing bundle file %s\n" f;
        exit 1))
    [ "original"; "computed"; "expected"; "diff" ];

  (* A matching fixture must not produce a bundle. *)
  if Diff_report.bundle_if_divergent ~out_dir:out "fixtures/stream_seed7.txt" then (
    prerr_endline "FAIL: unexpected divergence for stream_seed7.txt";
    exit 1);

  (* A malformed fixture must raise a catchable Stream_parser.Parse_error, not some other exception:
     the diff_report bin relies on catching exactly this (and Sys_error) to guard each fixture in
     the batch so one bad file cannot abort the whole run and silently lose later bundles. *)
  let bad = Filename.concat out "malformed.txt" in
  let oc = open_out bad in
  output_string oc "version 1\ncmd reg S0\n" (* no meta line *);
  close_out oc;
  (match Diff_report.bundle_if_divergent ~out_dir:out bad with
  | _ ->
      prerr_endline "FAIL: expected Parse_error for malformed fixture";
      exit 1
  | exception Stream_parser.Parse_error _ -> ());

  print_endline "diff_report: all tests passed"
