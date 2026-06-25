(* CLI: diff_report <out_dir> <fixture>...
   For each fixture, write a failure bundle to <out_dir> if the OCaml and C++ snapshots diverge.
   Exit 1 if any fixture diverged (so CI can upload <out_dir> on failure), else 0. *)

open Qsl_verifier

let () =
  match Array.to_list Sys.argv with
  | _ :: out_dir :: (_ :: _ as fixtures) ->
      let any =
        List.fold_left
          (fun acc f ->
            (* Guard each fixture: a malformed or unreadable one must not raise out of the fold and
               abort the whole batch, which would silently lose the divergence bundles for every
               later fixture -- exactly when CI needs them. Surface it as a failure (diverged=true)
               so the missing comparison forces a non-zero exit instead of disappearing. Matches the
               explicit Parse_error/Sys_error handling in verify_replay.ml and replay_snapshot.ml. *)
            let diverged =
              try
                let d = Diff_report.bundle_if_divergent ~out_dir f in
                if d then Printf.eprintf "divergence: %s -> bundle in %s\n" f out_dir;
                d
              with Stream_parser.Parse_error msg | Sys_error msg ->
                Printf.eprintf "cannot compare %s: %s\n" f msg;
                true
            in
            acc || diverged)
          false fixtures
      in
      exit (if any then 1 else 0)
  | _ ->
      prerr_endline "usage: diff_report <out_dir> <fixture>...";
      exit 2
