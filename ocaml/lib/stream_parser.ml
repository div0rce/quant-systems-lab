(* Parse an M15 differential-testing fixture into (meta, command list) for independent replay.
   Only the `meta` and `cmd` records are needed to replay; `evt`/`reject`/`snapshot`/`sym`/
   `level` lines are the C++ engine's reported output and are ignored here (the OCaml engine
   computes its own). The C++ snapshot comparison is M17. *)

open Replay_engine

exception Parse_error of string

let words line = String.split_on_char ' ' line |> List.filter (fun s -> s <> "")
let int_of t = match int_of_string_opt t with Some n -> n | None -> raise (Parse_error ("expected int: " ^ t))
let side_of = function "B" -> Buy | "S" -> Sell | s -> raise (Parse_error ("bad side: " ^ s))
let tif_of = function "GTC" -> GTC | "IOC" -> IOC | s -> raise (Parse_error ("bad tif: " ^ s))

(* Find the integer token immediately following `key` (used for meta fields). *)
let rec value_after key = function
  | a :: b :: _ when a = key -> int_of b
  | _ :: tl -> value_after key tl
  | [] -> raise (Parse_error ("missing meta field: " ^ key))

let parse_lines lines =
  let meta = ref None and commands = ref [] in
  let handle line =
    match words line with
    | [] -> ()
    | first :: _ when String.length first > 0 && first.[0] = '#' -> ()
    | "version" :: _ -> ()
    | "meta" :: _ as toks ->
        meta := Some { max_qty = value_after "max_qty" toks; max_notional = value_after "max_notional" toks }
    | [ "cmd"; "reg"; name ] -> commands := Reg name :: !commands
    | [ "cmd"; "limit"; sym; id; side; price; qty; tif ] ->
        commands := Limit (int_of sym, int_of id, side_of side, int_of price, int_of qty, tif_of tif) :: !commands
    | [ "cmd"; "market"; sym; id; side; qty ] ->
        commands := Market (int_of sym, int_of id, side_of side, int_of qty) :: !commands
    | [ "cmd"; "cancel"; sym; id ] -> commands := Cancel (int_of sym, int_of id) :: !commands
    | [ "cmd"; "modify"; sym; id; price; qty ] ->
        commands := Modify (int_of sym, int_of id, int_of price, int_of qty) :: !commands
    | "evt" :: _ | "reject" :: _ | "snapshot" :: _ | "sym" :: _ | "level" :: _ -> ()
    | other -> raise (Parse_error ("unrecognized line: " ^ String.concat " " other))
  in
  List.iter handle lines;
  match !meta with
  | None -> raise (Parse_error "fixture missing meta line (need max_qty/max_notional)")
  | Some m -> (m, List.rev !commands)

let parse_file path =
  let ic = open_in path in
  Fun.protect
    ~finally:(fun () -> close_in ic)
    (fun () ->
      let rec loop acc =
        match input_line ic with
        | line -> loop (line :: acc)
        | exception End_of_file -> List.rev acc
      in
      parse_lines (loop []))
