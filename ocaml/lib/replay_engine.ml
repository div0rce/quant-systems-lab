(* Independent OCaml replay engine (M16).

   Replays an exported command stream immutably and computes its own final snapshot, without
   trusting the C++ engine's emitted events. Semantics mirror the C++ system under test so the
   differential snapshot comparison (M17) can hold:
     - integer price ticks; bids best = highest, asks best = lowest;
     - price-time priority, FIFO within a level, fills at the resting maker's price;
     - GTC rests the remainder, IOC discards it, market orders never rest;
     - OrderId uniqueness is active-lifetime scoped (a later accept may reuse an id);
     - gateway risk checks (unknown symbol, duplicate active id, value checks) gate the engine;
     - every registered symbol appears in the snapshot (the engine try_emplaces a book).
   Sequence numbers count emitted events (accept + one per trade; cancel; modify + trades), so
   the final last_seq matches the C++ engine.

   This is invariant/differential checking infrastructure, not formal verification. *)

module IMap = Map.Make (Int)
module SMap = Map.Make (String)

type side = Buy | Sell
type tif = GTC | IOC

type command =
  | Reg of string
  | Limit of int * int * side * int * int * tif (* sym, id, side, price, qty, tif *)
  | Market of int * int * side * int (* sym, id, side, qty *)
  | Cancel of int * int (* sym, id *)
  | Modify of int * int * int * int (* sym, id, price, qty *)

type meta = { max_qty : int; max_notional : int }

type order = { id : int; qty : int }

type book = {
  bids : order list IMap.t; (* price -> FIFO orders (head = oldest) *)
  asks : order list IMap.t;
  locs : (side * int) IMap.t; (* id -> (side, resting price) *)
}

let empty_book = { bids = IMap.empty; asks = IMap.empty; locs = IMap.empty }
let contains b id = IMap.mem id b.locs

(* Match `qty` of a taker against the opposite-side levels. `taker_is_buy` selects asks
   (lowest first) vs bids (highest first); `limit` bounds crossing unless `is_market`.
   Returns (opp', locs', trades_rev, remaining). Trades are (taker, maker, price, qty). *)
let rec match_qty ~taker_id ~taker_is_buy ~limit ~is_market opp locs qty trades =
  if qty <= 0 || IMap.is_empty opp then (opp, locs, trades, qty)
  else
    let level_price, level =
      if taker_is_buy then IMap.min_binding opp else IMap.max_binding opp
    in
    let crosses =
      is_market || (if taker_is_buy then level_price <= limit else level_price >= limit)
    in
    if not crosses then (opp, locs, trades, qty)
    else
      let rec consume level locs qty trades =
        match level with
        | [] -> (level, locs, qty, trades)
        | maker :: rest ->
            if qty <= 0 then (level, locs, qty, trades)
            else
              let traded = min qty maker.qty in
              let trades = (taker_id, maker.id, level_price, traded) :: trades in
              let qty = qty - traded in
              let maker_qty = maker.qty - traded in
              if maker_qty = 0 then consume rest (IMap.remove maker.id locs) qty trades
              else ({ maker with qty = maker_qty } :: rest, locs, qty, trades)
      in
      let level', locs', qty', trades' = consume level locs qty trades in
      let opp' =
        if level' = [] then IMap.remove level_price opp else IMap.add level_price level' opp
      in
      match_qty ~taker_id ~taker_is_buy ~limit ~is_market opp' locs' qty' trades'

let rest_order b side price order =
  let add map =
    let level = match IMap.find_opt price map with Some l -> l | None -> [] in
    IMap.add price (level @ [ order ]) map
  in
  let locs = IMap.add order.id (side, price) b.locs in
  if side = Buy then { b with bids = add b.bids; locs } else { b with asks = add b.asks; locs }

let add_limit b id side price qty tif =
  let taker_is_buy = side = Buy in
  let opp = if taker_is_buy then b.asks else b.bids in
  let opp', locs', trades_rev, remaining =
    match_qty ~taker_id:id ~taker_is_buy ~limit:price ~is_market:false opp b.locs qty []
  in
  let b =
    if taker_is_buy then { b with asks = opp'; locs = locs' }
    else { b with bids = opp'; locs = locs' }
  in
  let b =
    if remaining > 0 && tif = GTC then rest_order b side price { id; qty = remaining } else b
  in
  (b, List.rev trades_rev)

let add_market b id side qty =
  let taker_is_buy = side = Buy in
  let opp = if taker_is_buy then b.asks else b.bids in
  let opp', locs', trades_rev, _remaining =
    match_qty ~taker_id:id ~taker_is_buy ~limit:0 ~is_market:true opp b.locs qty []
  in
  let b =
    if taker_is_buy then { b with asks = opp'; locs = locs' }
    else { b with bids = opp'; locs = locs' }
  in
  (b, List.rev trades_rev)

let cancel b id =
  match IMap.find_opt id b.locs with
  | None -> (b, false)
  | Some (side, price) ->
      let remove map =
        match IMap.find_opt price map with
        | None -> map
        | Some level ->
            let level' = List.filter (fun o -> o.id <> id) level in
            if level' = [] then IMap.remove price map else IMap.add price level' map
      in
      let locs = IMap.remove id b.locs in
      let b =
        if side = Buy then { b with bids = remove b.bids; locs }
        else { b with asks = remove b.asks; locs }
      in
      (b, true)

let resting_qty b side price id =
  let map = if side = Buy then b.bids else b.asks in
  match IMap.find_opt price map with
  | Some level -> (
      match List.find_opt (fun o -> o.id = id) level with Some o -> o.qty | None -> 0)
  | None -> 0

(* Book-level modify, mirroring the C++ engine: same-price quantity reduction keeps priority;
   a price change or quantity increase loses priority (cancel + re-add, which may cross). *)
let modify_book b id new_price new_qty =
  match IMap.find_opt id b.locs with
  | None -> (b, [])
  | Some (side, price0) ->
      if new_qty = 0 then (fst (cancel b id), [])
      else if new_price = price0 && new_qty <= resting_qty b side price0 id then
        let upd map =
          let level = IMap.find price0 map in
          IMap.add price0 (List.map (fun o -> if o.id = id then { o with qty = new_qty } else o) level) map
        in
        let b =
          if side = Buy then { b with bids = upd b.bids } else { b with asks = upd b.asks }
        in
        (b, [])
      else
        let b, _ = cancel b id in
        add_limit b id side new_price new_qty GTC

(* --- gateway risk (returns Some reason on reject, None on accept) --- *)
let check_limit_values meta price qty =
  if price <= 0 then Some "InvalidPrice"
  else if qty <= 0 then Some "InvalidQuantity"
  else if qty > meta.max_qty then Some "MaxQuantityExceeded"
  else if qty > meta.max_notional / price then Some "MaxNotionalExceeded"
  else None

let check_market meta qty =
  if qty <= 0 then Some "InvalidQuantity"
  else if qty > meta.max_qty then Some "MaxQuantityExceeded"
  else None

type state = {
  names : int SMap.t;
  next_id : int;
  books : book IMap.t; (* an entry exists for every registered symbol *)
  seq : int;
  trades : int;
  meta : meta;
}

let initial meta =
  { names = SMap.empty; next_id = 0; books = IMap.empty; seq = 0; trades = 0; meta }

let put_book st sym b ev fills =
  { st with books = IMap.add sym b st.books; seq = st.seq + ev; trades = st.trades + fills }

let apply st = function
  | Reg name -> (
      match SMap.find_opt name st.names with
      | Some _ -> st (* re-registering interns the same id; book already present *)
      | None ->
          {
            st with
            names = SMap.add name st.next_id st.names;
            next_id = st.next_id + 1;
            books = IMap.add st.next_id empty_book st.books;
          })
  | Limit (sym, id, side, price, qty, tif) -> (
      match IMap.find_opt sym st.books with
      | None -> st (* unknown symbol -> reject, no state change *)
      | Some b ->
          if contains b id then st (* duplicate active id -> reject *)
          else (
            match check_limit_values st.meta price qty with
            | Some _ -> st (* risk reject *)
            | None ->
                let b, trades = add_limit b id side price qty tif in
                put_book st sym b (1 + List.length trades) (List.length trades)))
  | Market (sym, id, side, qty) -> (
      match IMap.find_opt sym st.books with
      | None -> st
      | Some b ->
          if contains b id then st
          else (
            match check_market st.meta qty with
            | Some _ -> st
            | None ->
                let b, trades = add_market b id side qty in
                put_book st sym b (1 + List.length trades) (List.length trades)))
  | Cancel (sym, id) -> (
      match IMap.find_opt sym st.books with
      | None -> st
      | Some b -> if not (contains b id) then st else put_book st sym (fst (cancel b id)) 1 0)
  | Modify (sym, id, price, qty) -> (
      match IMap.find_opt sym st.books with
      | None -> st
      | Some b ->
          if not (contains b id) then st
          else if qty = 0 then
            let b, trades = modify_book b id price qty in
            put_book st sym b (1 + List.length trades) (List.length trades)
          else (
            match check_limit_values st.meta price qty with
            | Some _ -> st
            | None ->
                let b, trades = modify_book b id price qty in
                put_book st sym b (1 + List.length trades) (List.length trades)))

let replay meta commands = List.fold_left apply (initial meta) commands

(* --- snapshot --- *)
type level_view = { price : int; qty : int }

type sym_snapshot = {
  sym : int;
  best_bid : int option;
  best_ask : int option;
  order_count : int;
  bid_levels : level_view list; (* best (highest) first *)
  ask_levels : level_view list; (* best (lowest) first *)
}

type snapshot = { last_seq : int; n_trades : int; symbols : sym_snapshot list }

let level_views map ~descending =
  let bindings = IMap.bindings map in
  let ordered = if descending then List.rev bindings else bindings in
  List.map
    (fun (price, level) -> { price; qty = List.fold_left (fun a (o : order) -> a + o.qty) 0 level })
    ordered

let snapshot st =
  let symbols =
    IMap.bindings st.books
    |> List.map (fun (sym, b) ->
           {
             sym;
             best_bid = (match IMap.max_binding_opt b.bids with Some (p, _) -> Some p | None -> None);
             best_ask = (match IMap.min_binding_opt b.asks with Some (p, _) -> Some p | None -> None);
             order_count = IMap.cardinal b.locs;
             bid_levels = level_views b.bids ~descending:true;
             ask_levels = level_views b.asks ~descending:false;
           })
  in
  { last_seq = st.seq; n_trades = st.trades; symbols }
