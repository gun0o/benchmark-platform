package model

import (
	"encoding/json"
	"fmt"
	"io"
)

// ResultFunc is called once per result as it is parsed, in document order. The envelope
// it receives has its header filled in (run_id, machine, timestamps) but no results: a
// result is only handed over once the caller could write it somewhere.
type ResultFunc func(env *RunEnvelope, index int, r *Result) error

// StreamRunEnvelope decodes a run envelope from r, handing each result to fn as it is
// parsed rather than building the whole slice first. The returned envelope carries
// everything except `results`.
//
// Why stream: a 50,000-result envelope is 13.6 MB of JSON that decodes into ~90 MB of Go
// objects. Decoding it whole means that entire graph is live at once, per concurrent
// ingest. Handing results to the caller one at a time lets ingest keep only a bounded
// batch in memory, whatever the envelope's size.
//
// Field order: `machine` and `run_id` normally precede `results` (the engine writes them
// in schema order), and then nothing is buffered. If `results` arrives first, the results
// are buffered and replayed to fn after the header is complete, because ingest cannot
// write a measurement before it knows which run and machine it belongs to.
func StreamRunEnvelope(r io.Reader, maxResults int, fn ResultFunc) (*RunEnvelope, error) {
	dec := json.NewDecoder(r)
	dec.DisallowUnknownFields()

	tok, err := dec.Token()
	if err != nil {
		return nil, fmt.Errorf("run envelope: %w", err)
	}
	if delim, ok := tok.(json.Delim); !ok || delim != '{' {
		return nil, fmt.Errorf("run envelope: want a JSON object, got %v", tok)
	}

	env := &RunEnvelope{}
	var buffered []Result
	count := 0
	headerDone := func() bool { return env.RunID != "" && env.Machine.ID != "" }

	for dec.More() {
		keyTok, err := dec.Token()
		if err != nil {
			return nil, fmt.Errorf("run envelope: %w", err)
		}
		key, ok := keyTok.(string)
		if !ok {
			return nil, fmt.Errorf("run envelope: want a field name, got %v", keyTok)
		}
		switch key {
		case "schema_version":
			err = dec.Decode(&env.SchemaVersion)
		case "run_id":
			err = dec.Decode(&env.RunID)
		case "started_at":
			err = dec.Decode(&env.StartedAt)
		case "finished_at":
			err = dec.Decode(&env.FinishedAt)
		case "machine":
			err = dec.Decode(&env.Machine)
		case "argv":
			err = dec.Decode(&env.Argv)
		case "summary":
			err = dec.Decode(&env.Summary)
		case "results":
			count, buffered, err = streamResults(dec, env, maxResults, headerDone, fn)
		default:
			// The same strictness as DisallowUnknownFields, at the top level: a field the
			// API does not understand is a schema change nobody told it about.
			return nil, fmt.Errorf("run envelope: unknown field %q", key)
		}
		if err != nil {
			return nil, fmt.Errorf("%s: %w", key, err)
		}
	}
	if _, err := dec.Token(); err != nil { // closing '}'
		return nil, fmt.Errorf("run envelope: %w", err)
	}

	// Replay whatever had to be buffered because it arrived before the header.
	for i := range buffered {
		if err := fn(env, i, &buffered[i]); err != nil {
			return nil, err
		}
	}
	env.Results = nil
	if err := env.validateHeader(count); err != nil {
		return nil, err
	}
	return env, nil
}

func streamResults(dec *json.Decoder, env *RunEnvelope, maxResults int, headerDone func() bool, fn ResultFunc) (int, []Result, error) {
	tok, err := dec.Token()
	if err != nil {
		return 0, nil, err
	}
	if delim, ok := tok.(json.Delim); !ok || delim != '[' {
		return 0, nil, fmt.Errorf("want an array, got %v", tok)
	}
	var buffered []Result
	i := 0
	for dec.More() {
		if maxResults > 0 && i >= maxResults {
			return i, nil, &TooManyResultsError{Max: maxResults}
		}
		var res Result
		if err := dec.Decode(&res); err != nil {
			return i, nil, fmt.Errorf("results[%d]: %w", i, err)
		}
		var p problems
		res.validate(i, &p)
		if len(p.list) > 0 {
			return i, nil, &ValidationError{Problems: p.list}
		}
		if headerDone() {
			if err := fn(env, i, &res); err != nil {
				return i, nil, err
			}
		} else {
			buffered = append(buffered, res)
		}
		i++
	}
	if _, err := dec.Token(); err != nil { // closing ']'
		return i, nil, err
	}
	return i, buffered, nil
}

// TooManyResultsError is returned when an envelope carries more results than the ingest
// limit. The engine splits a larger run into several POSTs sharing one run_id.
type TooManyResultsError struct{ Max int }

func (e *TooManyResultsError) Error() string {
	return fmt.Sprintf("a run envelope may carry at most %d results; chunk the run", e.Max)
}

// validateHeader runs the envelope-level checks. Results are validated as they stream by.
func (e *RunEnvelope) validateHeader(resultCount int) error {
	var p problems
	if e.SchemaVersion != 1 {
		p.addf("schema_version must be 1, got %d", e.SchemaVersion)
	}
	if !uuidRe.MatchString(e.RunID) {
		p.addf("run_id %q is not a UUID v4", e.RunID)
	}
	started, errStart := ParseTimestamp(e.StartedAt)
	if errStart != nil {
		p.addf("started_at: %v", errStart)
	}
	finished, errFinish := ParseTimestamp(e.FinishedAt)
	if errFinish != nil {
		p.addf("finished_at: %v", errFinish)
	}
	if errStart == nil && errFinish == nil && finished.Before(started) {
		p.addf("finished_at %s is before started_at %s", e.FinishedAt, e.StartedAt)
	}
	e.Machine.validate(&p)
	if resultCount == 0 {
		p.addf("results is empty: a run with no measurements is not a run")
	}
	if len(p.list) > 0 {
		return &ValidationError{Problems: p.list}
	}
	return nil
}
