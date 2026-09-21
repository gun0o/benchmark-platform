package httpapi

import (
	"encoding/json"
	"net/http"
)

// errorBody is the JSON error envelope every non-2xx response carries:
//
//	{"error": {"code": "...", "message": "...", "details": [...]}}
type errorBody struct {
	Error errorPayload `json:"error"`
}

type errorPayload struct {
	Code    string   `json:"code"`
	Message string   `json:"message"`
	Details []string `json:"details,omitempty"`
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	// The body is already fully built; an encode error here means the client went away.
	_ = json.NewEncoder(w).Encode(v)
}

func writeError(w http.ResponseWriter, status int, code, msg string, details ...string) {
	writeJSON(w, status, errorBody{Error: errorPayload{Code: code, Message: msg, Details: details}})
}
