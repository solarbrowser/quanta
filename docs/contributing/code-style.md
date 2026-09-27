# Code Style

Comments explain *why*, not *what*. The code already says what it does if the names are good -- a comment restating that is noise.

What's worth writing down: a hidden constraint, a workaround for a specific bug, a decision made after measuring something, behavior that would surprise the next reader. If removing a comment wouldn't confuse anyone, it shouldn't be there.

## No Abstraction Ahead of Need

Three similar lines beat a helper built for a fourth case that doesn't exist yet. No speculative flexibility, no "might need this later."

## Keep It Dense and Honest

A comment that claims a reason should have one, not a guess dressed up as one.
