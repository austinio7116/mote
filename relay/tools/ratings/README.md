# Ratings backtests (2026-10-18)

How Elo, CUELO and ClaudeELO in the stats app were measured, and how to test a
trained model when there is enough history (Mark: retrain at ~3000 verified frames).

1. Copy the live results to a local folder, never anything written back:
   `ssh mote-relay 'sudo cat /var/lib/private/mote-relay/results.jsonl' > DIR/results.jsonl`
2. `python3 walk_features.py DIR` -- replays every line in time order through
   mote_stats_model.Model and, before each rated frame, records the CUELO
   rating gap and each play figure's gap (winner minus loser): DIR/feat.json.
   Nothing is seen before it happened.
3. `python3 fit_test.py DIR/feat.json` -- fits a logistic model on the first
   60% of frames and scores mean log loss and winner-picking on the last 40%.

Numbers on 2026-10-18 (792 frames): CUELO 0.6610 and ClaudeELO 0.6556 on the
last 40%; trained models 0.663-0.677 (worse). Add a model listing only if it
beats ClaudeELO here, with fixed weights (no training on the server).
