open Qsl_verifier
open Replay_engine

let big = { max_qty = 1_000_000; max_notional = 1_000_000_000 }
let fail msg = Printf.eprintf "FAIL: %s\n" msg; exit 1
let check name c = if not c then fail name
let snap ?(meta = big) cmds = snapshot (replay meta cmds)
let sym s i = List.find (fun (x : sym_snapshot) -> x.sym = i) s.symbols
let no_crossed s =
  List.for_all
    (fun (x : sym_snapshot) ->
      match (x.best_bid, x.best_ask) with Some b, Some a -> b < a | _ -> true)
    s.symbols

let () =
  (* 1. full cross at maker price *)
  let s = snap [ Reg "A"; Limit (0, 1, Sell, 100, 5, GTC); Limit (0, 2, Buy, 100, 5, GTC) ] in
  check "cross last_seq" (s.last_seq = 3);
  check "cross trades" (s.n_trades = 1);
  check "cross empty" ((sym s 0).order_count = 0 && (sym s 0).best_bid = None && (sym s 0).best_ask = None);

  (* 2. partial fill preserves resting priority/qty *)
  let s = snap [ Reg "A"; Limit (0, 1, Sell, 100, 5, GTC); Limit (0, 2, Sell, 100, 5, GTC); Limit (0, 3, Buy, 100, 3, GTC) ] in
  check "partial trades" (s.n_trades = 1);
  check "partial level qty" ((sym s 0).ask_levels = [ { price = 100; qty = 7 } ]);
  check "partial orders" ((sym s 0).order_count = 2);

  (* 3. cancel removes resting order *)
  let s = snap [ Reg "A"; Limit (0, 1, Buy, 100, 5, GTC); Cancel (0, 1) ] in
  check "cancel orders" ((sym s 0).order_count = 0);
  check "cancel last_seq" (s.last_seq = 2);

  (* 4a. modify reduce in place (same price, lower qty) *)
  let s = snap [ Reg "A"; Limit (0, 1, Sell, 100, 5, GTC); Modify (0, 1, 100, 3) ] in
  check "modify inplace qty" ((sym s 0).ask_levels = [ { price = 100; qty = 3 } ]);
  check "modify inplace no trade" (s.n_trades = 0 && s.last_seq = 2);

  (* 4b. modify reprice that crosses -> trade *)
  let s = snap [ Reg "A"; Limit (0, 1, Buy, 100, 5, GTC); Limit (0, 2, Sell, 106, 5, GTC); Modify (0, 2, 100, 5) ] in
  check "modify reprice cross trades" (s.n_trades = 1);
  check "modify reprice empty" ((sym s 0).order_count = 0);
  check "modify reprice last_seq" (s.last_seq = 4);

  (* 5. IOC discards remainder *)
  let s = snap [ Reg "A"; Limit (0, 1, Sell, 100, 3, GTC); Limit (0, 2, Buy, 100, 5, IOC) ] in
  check "ioc trades" (s.n_trades = 1);
  check "ioc no rest" ((sym s 0).order_count = 0 && s.last_seq = 3);

  (* 6a. market sweeps multiple levels, never rests *)
  let s = snap [ Reg "A"; Limit (0, 1, Sell, 100, 5, GTC); Limit (0, 2, Sell, 101, 5, GTC); Market (0, 3, Buy, 7) ] in
  check "market trades" (s.n_trades = 2);
  check "market remaining ask" ((sym s 0).ask_levels = [ { price = 101; qty = 3 } ]);
  check "market orders" ((sym s 0).order_count = 1);

  (* 6b. market on empty book: accepted, no fill, no rest *)
  let s = snap [ Reg "A"; Market (0, 1, Buy, 5) ] in
  check "market empty" ((sym s 0).order_count = 0 && s.n_trades = 0 && s.last_seq = 1);

  (* 7. duplicate active id rejected -> no second event/state *)
  let s = snap [ Reg "A"; Limit (0, 1, Buy, 100, 5, GTC); Limit (0, 1, Buy, 100, 5, GTC) ] in
  check "dup orders" ((sym s 0).order_count = 1);
  check "dup last_seq" (s.last_seq = 1);

  (* 8. risk-rejected (max qty) never rests *)
  let s = snap ~meta:{ max_qty = 8; max_notional = 1_000_000 } [ Reg "A"; Limit (0, 1, Buy, 100, 9, GTC) ] in
  check "reject orders" ((sym s 0).order_count = 0 && s.last_seq = 0);

  (* 9a. rejected id is reusable by a later accept *)
  let s = snap ~meta:{ max_qty = 8; max_notional = 1_000_000 } [ Reg "A"; Limit (0, 1, Buy, 100, 9, GTC); Limit (0, 1, Buy, 100, 5, GTC) ] in
  check "reuse-rejected rests" ((sym s 0).order_count = 1 && (sym s 0).best_bid = Some 100 && s.last_seq = 1);

  (* 9b. canceled id is reusable by a later accept (new lifetime, opposite side) *)
  let s = snap [ Reg "A"; Limit (0, 1, Buy, 100, 5, GTC); Cancel (0, 1); Limit (0, 1, Sell, 100, 5, GTC) ] in
  check "reuse-canceled rests" ((sym s 0).order_count = 1 && (sym s 0).best_ask = Some 100 && (sym s 0).best_bid = None);
  check "reuse-canceled last_seq" (s.last_seq = 3);

  (* 10. every registered symbol appears in the snapshot, even with no orders *)
  let s = snap [ Reg "A"; Reg "B"; Limit (0, 1, Buy, 100, 5, GTC) ] in
  check "two symbols present" (List.length s.symbols = 2);
  check "empty symbol present" ((sym s 1).order_count = 0 && (sym s 1).best_bid = None && (sym s 1).best_ask = None);

  (* 11. replay the committed C++ fixture: parses (incl. meta risk config) and is self-consistent *)
  let meta, commands = Stream_parser.parse_file "fixtures/stream_seed7.txt" in
  check "fixture meta max_qty parsed" (meta.max_qty = 8);
  let s = snapshot (replay meta commands) in
  check "fixture symbols" (List.length s.symbols = 4);
  check "fixture last_seq" (s.last_seq > 0);
  check "fixture no crossed book" (no_crossed s);

  print_endline "ocaml replay engine: all tests passed"
