(* Engine event-log domain types for the independent replay verifier.
   Records are kept in a single file-order stream so that order-id *lifetime* can be tracked:
   in this system OrderId uniqueness is scoped to currently-active resting orders, not global
   history, so a rejected or canceled id may be legally reused by a later accept. *)

type kind = Accept | Cancel | Modify

type event =
  | Lifecycle of kind * int * int * int
      (* kind, seq, symbol, order_id *)
  | Trade of int * int * int * int * int * int
      (* seq, symbol, taker, maker, price, qty *)

(* A fixture line: either a rejected new-order attempt (no sequence; never entered the engine)
   or an emitted engine event. Kept in file (sequence) order. *)
type record = Rej of int | Ev of event

let seq = function Lifecycle (_, s, _, _) -> s | Trade (s, _, _, _, _, _) -> s

type fixture = {
  records : record list; (* all records in file (sequence) order *)
  last_seq : int;        (* engine-reported final sequence number *)
  trades : int;          (* engine-reported trade count *)
}

let events_of fx = List.filter_map (function Ev e -> Some e | Rej _ -> None) fx.records
