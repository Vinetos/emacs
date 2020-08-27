;;; keyboard-tests.el --- Tests for keyboard.c -*- lexical-binding: t -*-

;; Copyright (C) 2017-2020 Free Software Foundation, Inc.

;; This file is part of GNU Emacs.

;; GNU Emacs is free software: you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.

;; GNU Emacs is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.

;;; Code:

(require 'ert)

(ert-deftest keyboard-unread-command-events ()
  "Test `unread-command-events'."
  (should (equal (progn (push ?\C-a unread-command-events)
                        (read-event nil nil 1))
                 ?\C-a))
  (should (equal (progn (run-with-timer
                         1 nil
                         (lambda () (push '(t . ?\C-b) unread-command-events)))
                        (read-event nil nil 2))
                 ?\C-b)))

(ert-deftest keyboard-lossage-size ()
  "Test `lossage-size'."
  (dolist (val (list 100 300 400 400 500 1000 700 300))
    (lossage-size val)
    (should (= val (lossage-size))))
  (let ((current-size (lossage-size)))
    ;; Set lossage size to 1 and 0 are equivalent
    (lossage-size 0)
    (should (zerop (lossage-size)))
    (lossage-size 1)
    (should (zerop (lossage-size)))
    (lossage-size current-size)
    ;; Argument must be a natural number
    (should-error (lossage-size -5))
    (should-error (lossage-size "200"))
    (should (= (lossage-size) current-size))))


(provide 'keyboard-tests)
;;; keyboard-tests.el ends here
