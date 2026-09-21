package store

import (
	"context"
	"fmt"
	"time"

	"github.com/matthewlee/benchmark-platform/api/internal/model"
)

// GroupBy is the dimension an aggregate query groups over. It is restricted to a closed
// set because it is interpolated into SQL as a column name.
type GroupBy string

const (
	GroupByThreadCount     GroupBy = "thread_count"
	GroupByWorkingSetBytes GroupBy = "working_set_bytes"
)

// ParseGroupBy validates the query parameter.
func ParseGroupBy(s string) (GroupBy, error) {
	switch s {
	case "", string(GroupByThreadCount):
		return GroupByThreadCount, nil
	case string(GroupByWorkingSetBytes):
		return GroupByWorkingSetBytes, nil
	default:
		return "", fmt.Errorf("group_by must be thread_count or working_set_bytes, got %q", s)
	}
}

// AggregateGroup is one group's summary. The statistics are the same ones the engine puts
// in a run's `summary`, computed here in SQL over whatever rows the filter selects:
// stddev is the sample standard deviation (n-1), median/p5/p95 use percentile_cont, which
// is numpy's "linear" interpolation, and cov = stddev / mean.
type AggregateGroup struct {
	Group  int64   `json:"group"`
	N      int64   `json:"n"`
	Mean   float64 `json:"mean"`
	Median float64 `json:"median"`
	Stddev float64 `json:"stddev"`
	CoV    float64 `json:"cov"`
	Min    float64 `json:"min"`
	P5     float64 `json:"p5"`
	P95    float64 `json:"p95"`
	Max    float64 `json:"max"`
}

// AggregateResponse is what GET /v1/aggregates returns.
type AggregateResponse struct {
	MachineID string           `json:"machine_id"`
	Metric    string           `json:"metric"`
	Workload  string           `json:"workload,omitempty"`
	Unit      string           `json:"unit"`
	GroupBy   GroupBy          `json:"group_by"`
	Groups    []AggregateGroup `json:"groups"`
}

// Aggregates computes per-group statistics. machine_id and metric are required by the
// handler: percentile_cont over an unfiltered 100K-row table is not a query this API
// should let a client ask for by accident.
func (s *Store) Aggregates(ctx context.Context, f MeasurementFilter, by GroupBy) (AggregateResponse, error) {
	res := AggregateResponse{
		MachineID: f.MachineID, Metric: f.Metric, Workload: f.Workload,
		Unit: model.UnitOf[f.Metric], GroupBy: by, Groups: []AggregateGroup{},
	}
	args := make([]any, 0, 6)
	where := f.where(&args)
	sql := fmt.Sprintf(`
		SELECT %[1]s AS grp,
		       count(*),
		       avg(value),
		       percentile_cont(0.5)  WITHIN GROUP (ORDER BY value),
		       coalesce(stddev_samp(value), 0),
		       min(value),
		       percentile_cont(0.05) WITHIN GROUP (ORDER BY value),
		       percentile_cont(0.95) WITHIN GROUP (ORDER BY value),
		       max(value)
		FROM measurements%[2]s
		GROUP BY %[1]s
		ORDER BY %[1]s`, string(by), where)

	rows, err := s.pool.Query(ctx, sql, args...)
	if err != nil {
		return res, fmt.Errorf("aggregates: %w", err)
	}
	defer rows.Close()
	for rows.Next() {
		var g AggregateGroup
		if err := rows.Scan(&g.Group, &g.N, &g.Mean, &g.Median, &g.Stddev, &g.Min,
			&g.P5, &g.P95, &g.Max); err != nil {
			return res, fmt.Errorf("scan aggregate: %w", err)
		}
		if g.Mean != 0 {
			g.CoV = g.Stddev / g.Mean
		}
		res.Groups = append(res.Groups, g)
	}
	return res, rows.Err()
}

// CompareSeries is one machine's aligned series in a comparison.
type CompareSeries struct {
	MachineID string    `json:"machine_id"`
	Hostname  string    `json:"hostname"`
	Groups    []int64   `json:"groups"`
	Median    []float64 `json:"median"`
	Mean      []float64 `json:"mean"`
	N         []int64   `json:"n"`
	// Ratio is this machine's median divided by the first machine's, per group. The first
	// machine's own ratios are 1. A group the first machine has no data for is 0.
	Ratio []float64 `json:"ratio"`
}

// CompareResponse is what GET /v1/compare returns.
type CompareResponse struct {
	Metric   string          `json:"metric"`
	Workload string          `json:"workload,omitempty"`
	Unit     string          `json:"unit"`
	GroupBy  GroupBy         `json:"group_by"`
	Baseline string          `json:"baseline"`
	Groups   []int64         `json:"groups"`
	Series   []CompareSeries `json:"series"`
}

// Compare returns one series per machine over the union of their group values, plus each
// machine's ratio to the first one. Aligning the series server-side is the point: the
// dashboard should not have to reconcile three machines that were swept at different
// thread counts.
func (s *Store) Compare(ctx context.Context, machineIDs []string, f MeasurementFilter, by GroupBy) (CompareResponse, error) {
	res := CompareResponse{
		Metric: f.Metric, Workload: f.Workload, Unit: model.UnitOf[f.Metric],
		GroupBy: by, Groups: []int64{}, Series: []CompareSeries{},
	}
	if len(machineIDs) == 0 {
		return res, nil
	}
	res.Baseline = machineIDs[0]

	f.MachineID = "" // the machine list replaces the single-machine filter
	args := []any{machineIDs}
	where := " WHERE m.machine_id = ANY($1)"
	extra := make([]any, 0, 4)
	if cond := f.where(&extra); cond != "" {
		// f.where numbered its placeholders from 1; shift them past $1.
		where += " AND " + shiftPlaceholders(cond[len(" WHERE "):], len(args))
		args = append(args, extra...)
	}
	sql := fmt.Sprintf(`
		SELECT m.machine_id, %[1]s AS grp, count(*), avg(m.value),
		       percentile_cont(0.5) WITHIN GROUP (ORDER BY m.value)
		FROM measurements m%[2]s
		GROUP BY m.machine_id, %[1]s
		ORDER BY %[1]s`, "m."+string(by), where)

	rows, err := s.pool.Query(ctx, sql, args...)
	if err != nil {
		return res, fmt.Errorf("compare: %w", err)
	}
	defer rows.Close()

	type cell struct {
		n      int64
		mean   float64
		median float64
	}
	data := map[string]map[int64]cell{}
	groupSet := map[int64]bool{}
	for rows.Next() {
		var machineID string
		var grp, n int64
		var mean, median float64
		if err := rows.Scan(&machineID, &grp, &n, &mean, &median); err != nil {
			return res, fmt.Errorf("scan compare: %w", err)
		}
		if data[machineID] == nil {
			data[machineID] = map[int64]cell{}
		}
		data[machineID][grp] = cell{n, mean, median}
		groupSet[grp] = true
	}
	if err := rows.Err(); err != nil {
		return res, err
	}

	groups := make([]int64, 0, len(groupSet))
	for g := range groupSet {
		groups = append(groups, g)
	}
	sortInt64(groups)
	res.Groups = groups

	hostnames, err := s.hostnames(ctx, machineIDs)
	if err != nil {
		return res, err
	}
	baseline := data[machineIDs[0]]
	for _, id := range machineIDs {
		series := CompareSeries{MachineID: id, Hostname: hostnames[id], Groups: groups}
		for _, g := range groups {
			c := data[id][g]
			series.N = append(series.N, c.n)
			series.Mean = append(series.Mean, c.mean)
			series.Median = append(series.Median, c.median)
			ratio := 0.0
			if b, ok := baseline[g]; ok && b.median != 0 {
				ratio = c.median / b.median
			}
			series.Ratio = append(series.Ratio, ratio)
		}
		res.Series = append(res.Series, series)
	}
	return res, nil
}

func (s *Store) hostnames(ctx context.Context, ids []string) (map[string]string, error) {
	out := map[string]string{}
	rows, err := s.pool.Query(ctx, `SELECT id, hostname FROM machines WHERE id = ANY($1)`, ids)
	if err != nil {
		return nil, fmt.Errorf("hostnames: %w", err)
	}
	defer rows.Close()
	for rows.Next() {
		var id, host string
		if err := rows.Scan(&id, &host); err != nil {
			return nil, err
		}
		out[id] = host
	}
	return out, rows.Err()
}

// TrialsResponse is the compact shape the Trials page loads: parallel arrays rather than
// 10,000 objects with the same six keys.
type TrialsResponse struct {
	MachineID  string       `json:"machine_id"`
	Metric     string       `json:"metric"`
	Unit       string       `json:"unit"`
	N          int          `json:"n"`
	Downsample string       `json:"downsample,omitempty"`
	Points     [][3]any     `json:"points"` // [trial, value, recorded_at]
	Raw        []TrialPoint `json:"-"`
}

// TrialPoint is one trial, before it is flattened into the compact form.
type TrialPoint struct {
	Trial      int
	Value      float64
	RecordedAt time.Time
}

// Trials returns individual trial values for one configuration, in trial order.
func (s *Store) Trials(ctx context.Context, f MeasurementFilter) ([]TrialPoint, error) {
	args := make([]any, 0, 6)
	where := f.where(&args)
	limit := f.Limit
	if limit <= 0 {
		limit = 10_000
	}
	args = append(args, limit)
	sql := fmt.Sprintf(`
		SELECT trial, value, recorded_at FROM measurements%s
		ORDER BY recorded_at, trial LIMIT $%d`, where, len(args))
	rows, err := s.pool.Query(ctx, sql, args...)
	if err != nil {
		return nil, fmt.Errorf("trials: %w", err)
	}
	defer rows.Close()
	out := make([]TrialPoint, 0, min(limit, 4096))
	for rows.Next() {
		var p TrialPoint
		if err := rows.Scan(&p.Trial, &p.Value, &p.RecordedAt); err != nil {
			return nil, fmt.Errorf("scan trial: %w", err)
		}
		p.RecordedAt = p.RecordedAt.UTC()
		out = append(out, p)
	}
	return out, rows.Err()
}

func sortInt64(v []int64) {
	for i := 1; i < len(v); i++ {
		for j := i; j > 0 && v[j] < v[j-1]; j-- {
			v[j], v[j-1] = v[j-1], v[j]
		}
	}
}

// shiftPlaceholders renumbers $1, $2 ... by n, so a condition built independently can be
// appended after arguments that are already numbered.
func shiftPlaceholders(cond string, n int) string {
	out := make([]byte, 0, len(cond)+8)
	for i := 0; i < len(cond); i++ {
		if cond[i] != '$' {
			out = append(out, cond[i])
			continue
		}
		j := i + 1
		num := 0
		for j < len(cond) && cond[j] >= '0' && cond[j] <= '9' {
			num = num*10 + int(cond[j]-'0')
			j++
		}
		if num == 0 {
			out = append(out, cond[i])
			continue
		}
		out = append(out, []byte(fmt.Sprintf("$%d", num+n))...)
		i = j - 1
	}
	return string(out)
}
