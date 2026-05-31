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
            let diverged = Diff_report.bundle_if_divergent ~out_dir f in
            if diverged then Printf.eprintf "divergence: %s -> bundle in %s\n" f out_dir;
            acc || diverged)
          false fixtures
      in
      exit (if any then 1 else 0)
  | _ ->
      prerr_endline "usage: diff_report <out_dir> <fixture>...";
      exit 2
