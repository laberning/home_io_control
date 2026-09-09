/*
 * Mobile navigation drawer (screens < 768px).
 *
 * doxygen-awesome's sidebar-only theme sets `#side-nav { display: none }` below
 * 768px and hands navigation to the top menu bar. menu.js builds that bar from
 * menudata.js, which is generated from the <navindex> in DoxygenLayout.xml and
 * therefore only carries "All Pages" -- the nested page hierarchy exists solely
 * in #nav-tree. This script reveals #nav-tree as a drawer instead, so every
 * documentation page is reachable on a phone.
 *
 * Why a purpose-built toggle instead of the theme's own #main-menu-state
 * checkbox (which label.main-menu-btn drives natively):
 *
 *   * menu.js listens for `change` on that checkbox and, in the slide callback,
 *     re-runs initResizable() from navtree.js. That re-registers listeners,
 *     re-runs resizeHeight() (which does scrollIntoView() when the URL has a
 *     hash), re-assigns window.location.hash, and rewrites the sidebar-width
 *     cookie from whatever width #side-nav happens to have -- on a phone that
 *     clamps to ~43px and corrupts the *desktop* sidebar width.
 *   * menu.js's resetState() is bound to window resize and force-sets
 *     `mainMenuState.checked = false` whenever window.innerWidth differs from
 *     the previous value while below 768px. Any toggle that changes the
 *     document's scroll geometry (scrollbar appearing, iOS zoom-to-fit on
 *     horizontal overflow) therefore closes itself again immediately.
 *
 * So we detach label.main-menu-btn from the checkbox (drop `for`), drive a
 * `hioc-mobile-nav-open` class on <body> ourselves, and let custom.css render
 * #side-nav as a position:fixed panel. Because #side-nav is already out of flow
 * (navtree.css: position:absolute) and nothing else is hidden, opening and
 * closing the drawer does not change the document's layout at all: no scroll
 * anchoring, no scrollbar flicker, no resize event, nothing that can re-close
 * it or move the page.
 *
 * Kept deliberately small and dependency-free; loaded from header.html.
 */
(function () {
    'use strict';

    /* Must stay in sync with the @media block in custom.css and with menu.js's
       own MOBILE_WIDTH (768). */
    var MOBILE_MEDIA = '(max-width: 767px)';
    var OPEN_CLASS = 'hioc-mobile-nav-open';
    /* custom.css hangs the fixed panel off this; see position() below. */
    var TOP_VAR = '--hioc-mobile-nav-top';

    function init() {
        var top = document.getElementById('top');
        var button = document.querySelector('label.main-menu-btn');
        var sideNav = document.getElementById('side-nav');
        var navTree = document.getElementById('nav-tree');
        if (!top || !button || !sideNav || !navTree) {
            return;
        }

        /* Take the hamburger away from #main-menu-state so that tapping it can
           no longer fire menu.js's `change` handler (see the header comment).
           The label keeps its look; it just becomes our button. */
        button.removeAttribute('for');
        button.setAttribute('role', 'button');
        button.setAttribute('aria-controls', 'side-nav');
        button.setAttribute('aria-expanded', 'false');

        var mobile = window.matchMedia(MOBILE_MEDIA);

        /* tabs.css parks .main-menu-btn at `top: -99999px` from 768px up, so it
           may only join the tab order while it is actually on screen -- a
           focusable off-screen element would make the desktop page jump. */
        function syncTabbable() {
            if (mobile.matches) {
                button.setAttribute('tabindex', '0');
            } else {
                button.removeAttribute('tabindex');
            }
        }
        syncTabbable();

        function isOpen() {
            return document.body.classList.contains(OPEN_CLASS);
        }

        /* The panel is position:fixed and starts just below the header, so it
           needs the header's current bottom edge in viewport coordinates. On a
           phone the body is not the scroller (navtree.js pins #doc-content to a
           viewport-tall box with its own overflow), so this is normally just
           #top's height -- the max() only matters if the body ever does scroll,
           in which case the panel simply covers the full viewport. */
        function position() {
            var bottom = Math.max(0, Math.round(top.getBoundingClientRect().bottom));
            document.documentElement.style.setProperty(TOP_VAR, bottom + 'px');
        }

        /* Centre the current page in the tree. Only touches #nav-tree's own
           scrollTop -- never scrollIntoView(), which would also scroll every
           scrollable ancestor and move the page. */
        function revealSelected() {
            var selected = navTree.querySelector('#selected');
            if (!selected) {
                return;
            }
            var itemRect = selected.getBoundingClientRect();
            var treeRect = navTree.getBoundingClientRect();
            navTree.scrollTop += (itemRect.top - treeRect.top) - (treeRect.height / 2);
        }

        function setOpen(open) {
            if (open) {
                position();
            }
            document.body.classList.toggle(OPEN_CLASS, open);
            button.setAttribute('aria-expanded', open ? 'true' : 'false');
            if (open) {
                revealSelected();
            }
        }

        function toggle(event) {
            event.preventDefault();
            setOpen(!isOpen());
        }

        button.addEventListener('click', toggle);
        button.addEventListener('keydown', function (event) {
            if (event.key === 'Enter' || event.key === ' ') {
                toggle(event);
            }
        });

        document.addEventListener('keydown', function (event) {
            if (event.key === 'Escape' && isOpen()) {
                setOpen(false);
            }
        });

        /* Close after picking a page -- but only for links that really
           navigate. navtree.js builds two kinds of <a> that must not close the
           drawer, and both are marked by their href: the expand chevron
           (`<a href="javascript:void(0)"><span class="arrow">`, createIndent)
           and group nodes that have children but no page of their own
           (a.nolink, e.g. the "Documentation" usergroup from
           DoxygenLayout.xml). */
        sideNav.addEventListener('click', function (event) {
            var link = event.target.closest && event.target.closest('a');
            var href = link && link.getAttribute('href');
            if (href && href.slice(0, 11) !== 'javascript:') {
                setOpen(false);
            }
        });

        window.addEventListener('resize', function () {
            if (isOpen()) {
                position();
            }
        });

        /* Crossing back to the desktop layout drops the drawer state so the
           >= 768px rendering is exactly the stock one. */
        var onModeChange = function () {
            syncTabbable();
            if (!mobile.matches) {
                setOpen(false);
            }
        };
        if (mobile.addEventListener) {
            mobile.addEventListener('change', onModeChange);
        } else if (mobile.addListener) {
            mobile.addListener(onModeChange);
        }
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }
})();
