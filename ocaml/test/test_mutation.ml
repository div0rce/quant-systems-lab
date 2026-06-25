(* Oracle mutation testing (issue #48). The differential layer detects a divergence iff the two
   snapshots render to different lines (`snapshot_to_lines`, the exact comparison M17 uses). This
   test takes one representative snapshot and applies a single-field mutation for every snapshot
   field, last_seq, trade count, symbol id, best bid/ask, order count, and bid/ask levels, asserting each
   mutation changes the rendered lines. If any field stopped contributing to the comparison, a
   real C++/OCaml divergence in it could pass silently; this fails instead.

   Complements the hand-authored negative fixtures (issue #36) by mutating the snapshot object
   directly and covering every field programmatically. *)

open Qsl_verifier
open Replay_engine

let base =
  {
    last_seq = 5;
    n_trades = 2;
    symbols =
      [
        {
          sym = 0;
          best_bid = Some 100;
          best_ask = Some 105;
          order_count = 3;
          bid_levels = [ { price = 100; qty = 4 } ];
          ask_levels = [ { price = 105; qty = 2 } ];
        };
      ];
  }

(* Replace symbol 0's fields. *)
let map_sym0 s f =
  match s.symbols with x :: rest -> { s with symbols = f x :: rest } | [] -> s

let mutations =
  [
    ("last_seq", fun s -> { s with last_seq = s.last_seq + 1 });
    ("n_trades", fun s -> { s with n_trades = s.n_trades + 1 });
    ("sym id", fun s -> map_sym0 s (fun x -> { x with sym = x.sym + 1 }));
    ("best_bid value", fun s -> map_sym0 s (fun x -> { x with best_bid = Some 99 }));
    ("best_bid None", fun s -> map_sym0 s (fun x -> { x with best_bid = None }));
    ("best_ask value", fun s -> map_sym0 s (fun x -> { x with best_ask = Some 106 }));
    ("best_ask None", fun s -> map_sym0 s (fun x -> { x with best_ask = None }));
    ("order_count", fun s -> map_sym0 s (fun x -> { x with order_count = x.order_count + 1 }));
    ("bid_levels qty", fun s -> map_sym0 s (fun x -> { x with bid_levels = [ { price = 100; qty = 9 } ] }));
    ("bid_levels price", fun s -> map_sym0 s (fun x -> { x with bid_levels = [ { price = 99; qty = 4 } ] }));
    ("ask_levels qty", fun s -> map_sym0 s (fun x -> { x with ask_levels = [ { price = 105; qty = 9 } ] }));
    ("ask_levels price", fun s -> map_sym0 s (fun x -> { x with ask_levels = [ { price = 104; qty = 2 } ] }));
  ]

let () =
  let base_lines = snapshot_to_lines base in
  (* Sanity: an unmutated snapshot is not flagged as divergent. *)
  if snapshot_to_lines base <> base_lines then (
    prerr_endline "FAIL: base snapshot is not stable under rendering";
    exit 1);
  List.iter
    (fun (name, mutate) ->
      if snapshot_to_lines (mutate base) = base_lines then (
        Printf.eprintf "FAIL: mutation '%s' not detected by the differential comparison\n" name;
        exit 1))
    mutations;
  Printf.printf "mutation test: all %d snapshot mutations detected\n" (List.length mutations)
