(* Engine event-log domain types for the independent replay verifier.
   A normalized fixture (exported by the C++ qsl-export-fixture tool) is parsed into this
   shape; the verifier then re-derives invariants from it without trusting the C++ engine. *)

type kind = Accept | Cancel | Modify

type event =
  | Lifecycle of kind * int * int * int
      (* kind, seq, symbol, order_id *)
  | Trade of int * int * int * int * int * int
      (* seq, symbol, taker, maker, price, qty *)

let seq = function Lifecycle (_, s, _, _) -> s | Trade (s, _, _, _, _, _) -> s

type fixture = {
  rejects : int list;  (* rejected new-order ids (never reached the engine) *)
  events : event list; (* engine events in emission (sequence) order *)
  last_seq : int;      (* engine-reported final sequence number *)
  trades : int;        (* engine-reported trade count *)
}
