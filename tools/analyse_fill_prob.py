#!/usr/bin/env python3
import sys, re
import matplotlib.pyplot as plt

def parse_table(lines):
    bins = []
    for line in lines:
        m = re.match(r'\[([\d.]+)-([\d.]+)\)\s+(\d+)\s+(\d+)\s+([\d.]+)%', line)
        if m:
            lo, hi, placed, filled, actual = float(m.group(1)), float(m.group(2)), int(m.group(3)), int(m.group(4)), float(m.group(5))
            bins.append(((lo+hi)/2, actual, placed))
    return bins

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python analyse_fill_prob.py <log_file>")
        sys.exit(1)
    with open(sys.argv[1]) as f:
        lines = f.readlines()
    bins = parse_table(lines)
    if not bins:
        print("No validation data found.")
        sys.exit(1)
    midpoints, actuals, counts = zip(*bins)
    plt.figure()
    plt.scatter(midpoints, actuals, s=[c/10 for c in counts], alpha=0.7)
    plt.plot([0,1],[0,100], 'k--')
    plt.xlabel('Predicted fill probability')
    plt.ylabel('Actual fill %')
    plt.title('Fill Probability Calibration')
    plt.grid(True)
    plt.savefig('fill_prob_calibration.png')
    plt.show()
