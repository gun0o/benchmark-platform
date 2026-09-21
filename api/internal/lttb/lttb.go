// Package lttb implements Largest-Triangle-Three-Buckets downsampling.
//
// A 10,000-point trial series drawn into 800 pixels has ~12 points per pixel. Dropping
// every N-th point loses exactly the spikes a variance study is about; LTTB keeps the
// point in each bucket that forms the largest triangle with its neighbours, which is a
// cheap way of keeping the extremes that define the visual shape.
//
// The dashboard does this client-side too (M6.3); the server-side copy exists so a slow
// client can ask for fewer points over the wire.
package lttb

// Point is one (x, y) sample. x is the trial index or a timestamp in any monotonic unit.
type Point struct {
	X float64
	Y float64
}

// Downsample reduces data to at most threshold points, keeping the first and last.
// threshold < 3 or >= len(data) returns data unchanged.
func Downsample(data []Point, threshold int) []Point {
	n := len(data)
	if threshold >= n || threshold < 3 {
		return data
	}
	out := make([]Point, 0, threshold)
	out = append(out, data[0])

	// Bucket size, excluding the first and last points which are always kept.
	every := float64(n-2) / float64(threshold-2)
	a := 0 // index of the previously selected point
	for i := 0; i < threshold-2; i++ {
		// The average of the next bucket is the triangle's third vertex.
		nextStart := int(float64(i+1)*every) + 1
		nextEnd := int(float64(i+2)*every) + 1
		if nextEnd > n {
			nextEnd = n
		}
		avgX, avgY, count := 0.0, 0.0, 0
		for j := nextStart; j < nextEnd; j++ {
			avgX += data[j].X
			avgY += data[j].Y
			count++
		}
		if count == 0 {
			avgX, avgY = data[n-1].X, data[n-1].Y
		} else {
			avgX /= float64(count)
			avgY /= float64(count)
		}

		start := int(float64(i)*every) + 1
		end := int(float64(i+1)*every) + 1
		if end > n-1 {
			end = n - 1
		}
		best, bestArea := start, -1.0
		for j := start; j < end; j++ {
			area := triangleArea(data[a], data[j], Point{avgX, avgY})
			if area > bestArea {
				bestArea, best = area, j
			}
		}
		out = append(out, data[best])
		a = best
	}
	return append(out, data[n-1])
}

func triangleArea(a, b, c Point) float64 {
	area := (a.X-c.X)*(b.Y-a.Y) - (a.X-b.X)*(c.Y-a.Y)
	if area < 0 {
		return -area
	}
	return area
}
