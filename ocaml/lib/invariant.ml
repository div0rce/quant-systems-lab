(* Independent replay invariants, re-derived from the exported event log. The verifier does
   not trust the C++ engine: each property is recomputed from the raw records. This is
   invariant checking, not formal verification or a proof of correctness. *)

open Event
module IntSet = Set.Make (Int)

(* A check result: name, ok, and a detail string (empty when ok). *)
type result = string * bool * string

let monotonic_seq fx : result =
  let rec go prev = function
    | [] -> (true, "")
    | e :: tl ->
        let s = seq e in
        if s > prev then go s tl else (false, Printf.sprintf "seq %d not > previous %d" s prev)
  in
  let ok, detail = go 0 fx.events in
  ("sequence strictly increasing", ok, detail)

let positive_qty fx : result =
  match
    List.find_opt (function Trade (_, _, _, _, _, q) -> q <= 0 | _ -> false) fx.events
  with
  | Some (Trade (s, _, _, _, _, q)) ->
      ("positive trade quantity", false, Printf.sprintf "seq %d has qty %d" s q)
  | _ -> ("positive trade quantity", true, "")

let canceled_cannot_trade fx : result =
  let rec go canceled = function
    | [] -> (true, "")
    | Lifecycle (Cancel, _, _, id) :: tl -> go (IntSet.add id canceled) tl
    | Trade (s, _, tk, mk, _, _) :: tl ->
        if IntSet.mem mk canceled || IntSet.mem tk canceled then
          (false, Printf.sprintf "canceled order traded at seq %d" s)
        else go canceled tl
    | _ :: tl -> go canceled tl
  in
  let ok, detail = go IntSet.empty fx.events in
  ("canceled order cannot later trade", ok, detail)

let rejected_cannot_rest fx : result =
  let rejected = IntSet.of_list fx.rejects in
  let touches id = IntSet.mem id rejected in
  let bad =
    List.find_opt
      (function
        | Lifecycle (_, _, _, id) -> touches id
        | Trade (_, _, tk, mk, _, _) -> touches tk || touches mk)
      fx.events
  in
  match bad with
  | Some _ -> ("rejected order never rests or trades", false, "a rejected id appears in an event")
  | None -> ("rejected order never rests or trades", true, "")

let summary_consistent fx : result =
  let max_seq = List.fold_left (fun m e -> max m (seq e)) 0 fx.events in
  let trade_count = List.fold_left (fun n e -> match e with Trade _ -> n + 1 | _ -> n) 0 fx.events in
  if max_seq <> fx.last_seq then
    ("summary matches event log", false, Printf.sprintf "max seq %d <> last_seq %d" max_seq fx.last_seq)
  else if trade_count <> fx.trades then
    ("summary matches event log", false, Printf.sprintf "trade count %d <> reported %d" trade_count fx.trades)
  else ("summary matches event log", true, "")

let run fx =
  [
    monotonic_seq fx;
    positive_qty fx;
    canceled_cannot_trade fx;
    rejected_cannot_rest fx;
    summary_consistent fx;
  ]

let all_ok checks = List.for_all (fun (_, ok, _) -> ok) checks
