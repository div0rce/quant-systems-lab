(* Independent replay invariants, re-derived from the exported event log. The verifier does
   not trust the C++ engine: each property is recomputed from the raw records.

   OrderId lifetime: in this system OrderId uniqueness is scoped to currently-active resting
   orders, not to global history. A rejected attempt never enters the engine, and a canceled
   order leaves the active set, so the same numeric id may be legally reused by a later accept.
   Lifetime checks therefore track per-id state in sequence order, and an `accept` starts a
   fresh active lifetime that clears any prior rejected/canceled state.

   This is invariant checking, not formal verification or a proof of correctness. *)

open Event

(* A check result: name, ok, and a detail string (empty when ok). *)
type result = string * bool * string

let monotonic_seq fx : result =
  let rec go prev = function
    | [] -> (true, "")
    | e :: tl ->
        let s = seq e in
        if s > prev then go s tl else (false, Printf.sprintf "seq %d not > previous %d" s prev)
  in
  let ok, detail = go 0 (events_of fx) in
  ("sequence strictly increasing", ok, detail)

let positive_qty fx : result =
  match
    List.find_opt (function Trade (_, _, _, _, _, q) -> q <= 0 | _ -> false) (events_of fx)
  with
  | Some (Trade (s, _, _, _, _, q)) ->
      ("positive trade quantity", false, Printf.sprintf "seq %d has qty %d" s q)
  | _ -> ("positive trade quantity", true, "")

(* Single sequence-order pass tracking per-id lifetime state:
     `Active   - an accept established a live order (cleared by cancel / a fresh reject only
                 when not active)
     `Canceled - canceled; trading it (without a later accept) is illegal
     `Rejected - the last attempt for this id was rejected and never accepted; it must not
                 rest (cancel/modify) or trade until a later accept reuses the id
   Returns the canceled-cannot-trade and rejected-cannot-rest results together. *)
let lifetime_checks fx : result * result =
  let st = Hashtbl.create 256 in
  let state id = match Hashtbl.find_opt st id with Some s -> s | None -> `Unknown in
  let cv = ref "" and rv = ref "" in
  let note r msg = if !r = "" then r := msg in
  let on_active_use id s =
    match state id with
    | `Canceled -> note cv (Printf.sprintf "canceled order %d acted at seq %d" id s)
    | `Rejected -> note rv (Printf.sprintf "rejected order %d rests/trades at seq %d" id s)
    | _ -> ()
  in
  let step = function
    | Rej id -> if state id <> `Active then Hashtbl.replace st id `Rejected
    | Ev (Lifecycle (Accept, _, _, id)) -> Hashtbl.replace st id `Active (* new lifetime *)
    | Ev (Lifecycle (Modify, s, _, id)) ->
        on_active_use id s;
        Hashtbl.replace st id `Active
    | Ev (Lifecycle (Cancel, s, _, id)) ->
        on_active_use id s;
        Hashtbl.replace st id `Canceled
    | Ev (Trade (s, _, tk, mk, _, _)) ->
        on_active_use tk s;
        on_active_use mk s
  in
  List.iter step fx.records;
  ( ("canceled order cannot later trade", !cv = "", !cv),
    ("rejected order never rests or trades", !rv = "", !rv) )

let summary_consistent fx : result =
  let events = events_of fx in
  let max_seq = List.fold_left (fun m e -> max m (seq e)) 0 events in
  let trade_count = List.fold_left (fun n e -> match e with Trade _ -> n + 1 | _ -> n) 0 events in
  if max_seq <> fx.last_seq then
    ("summary matches event log", false, Printf.sprintf "max seq %d <> last_seq %d" max_seq fx.last_seq)
  else if trade_count <> fx.trades then
    ("summary matches event log", false, Printf.sprintf "trade count %d <> reported %d" trade_count fx.trades)
  else ("summary matches event log", true, "")

let run fx =
  let canceled_chk, rejected_chk = lifetime_checks fx in
  [ monotonic_seq fx; positive_qty fx; canceled_chk; rejected_chk; summary_consistent fx ]

let all_ok checks = List.for_all (fun (_, ok, _) -> ok) checks
