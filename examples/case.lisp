(defun (day-of day)
    (case day
      (1 "Monday")
      (2 "Tuesday")
      (3 "Wednesday")
      (4 "Thursday")
      (5 "Friday")
      (6 "Saturday")
      (7 "Sunday")))
(print "Day 2 is " (day-of 2))
(print "Day 5 is " (day-of 5))
(print "Day 1 is " (day-of 1))
(print "Day 13 is " (day-of 11))
