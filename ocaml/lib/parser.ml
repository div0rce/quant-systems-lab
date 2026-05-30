(* Parser for the normalized textual fixture format (see docs/ocaml_verifier.md).
   One record per line; tokens are space-separated; lines starting with '#' are comments. *)

open Event

exception Parse_error of string

let words line = String.split_on_char ' ' line |> List.filter (fun s -> s <> "")

let int_of tok =
  match int_of_string_opt tok with
  | Some n -> n
  | None -> raise (Parse_error ("expected integer, got: " ^ tok))

let parse_lines lines =
  let rejects = ref [] and events = ref [] and last_seq = ref 0 and trades = ref 0 in
  let handle line =
    match words line with
    | [] -> ()
    | first :: _ when String.length first > 0 && first.[0] = '#' -> ()
    | [ "v"; _ ] -> ()
    | "meta" :: _ -> ()
    | [ "reject"; id; _reason ] -> rejects := int_of id :: !rejects
    | [ "accept"; s; sym; id ] ->
        events := Lifecycle (Accept, int_of s, int_of sym, int_of id) :: !events
    | [ "cancel"; s; sym; id ] ->
        events := Lifecycle (Cancel, int_of s, int_of sym, int_of id) :: !events
    | [ "modify"; s; sym; id ] ->
        events := Lifecycle (Modify, int_of s, int_of sym, int_of id) :: !events
    | [ "trade"; s; sym; tk; mk; px; q ] ->
        events :=
          Trade (int_of s, int_of sym, int_of tk, int_of mk, int_of px, int_of q) :: !events
    | [ "summary"; "last_seq"; ls; "trades"; t ] ->
        last_seq := int_of ls;
        trades := int_of t
    | other -> raise (Parse_error ("unrecognized line: " ^ String.concat " " other))
  in
  List.iter handle lines;
  {
    rejects = List.rev !rejects;
    events = List.rev !events;
    last_seq = !last_seq;
    trades = !trades;
  }

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
