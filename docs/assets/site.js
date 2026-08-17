// Mobile sidebar toggle + active nav highlighting
(function () {
  const btn = document.querySelector('.hamburger');
  const sidebar = document.querySelector('.sidebar');
  const backdrop = document.querySelector('.backdrop');
  if (btn && sidebar && backdrop) {
    const toggle = () => {
      sidebar.classList.toggle('open');
      backdrop.classList.toggle('show');
    };
    btn.addEventListener('click', toggle);
    backdrop.addEventListener('click', toggle);
  }

  // Highlight active nav link based on current path
  const path = location.pathname.split('/').pop() || 'index.html';
  document.querySelectorAll('.nav a').forEach(a => {
    const href = a.getAttribute('href');
    if (href === path || (path === '' && href === 'index.html')) {
      a.classList.add('active');
    }
  });

  // Run highlight.js if present
  if (window.hljs) {
    document.querySelectorAll('pre code').forEach(el => {
      if (!el.classList.contains('ascii') && !el.parentElement.classList.contains('ascii')) {
        window.hljs.highlightElement(el);
      }
    });
  }
})();
