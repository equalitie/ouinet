;; Use these channels to prepare a Guix profile
;; for starting environments in which to build Ouinet
;; with the packages listed in `manifest.scm`.
(list (channel
        (name 'guix)
        (url "https://git.savannah.gnu.org/git/guix.git")
        (commit
          "79154f0a09ad748839f88120ddd61a0e1e147b5e")))
