// Conversation page interactions; message loading can be added without changing the shell.
var convoPath = window.location.pathname;

document.addEventListener('DOMContentLoaded', function () {
    const container = document.getElementById('convos-container');
    const sidebarToggle = document.getElementById('navToggle');
    const mobileToggle = document.getElementById('mobileNavToggle');
    const searchInput = document.getElementById('convoSearch');
    const emptyState = document.getElementById('convoEmpty');

    function setSidebarOpen(isOpen) {
        if (!container) return;
        container.classList.toggle('sidebar-collapsed', !isOpen);
        container.classList.toggle('sidebar-open', isOpen);
        if (sidebarToggle) {
            sidebarToggle.setAttribute('aria-expanded', String(isOpen));
            sidebarToggle.setAttribute('aria-label', isOpen ? 'Collapse conversations sidebar' : 'Expand conversations sidebar');
        }
    }

    if (sidebarToggle) sidebarToggle.addEventListener('click', function () {
        setSidebarOpen(!container.classList.contains('sidebar-open'));
    });

    if (mobileToggle) mobileToggle.addEventListener('click', function () {
        setSidebarOpen(true);
    });

    if (searchInput) searchInput.addEventListener('input', function () {
        const query = searchInput.value.trim().toLowerCase();
        const items = Array.from(document.querySelectorAll('#convoList > li:not(.convo-empty)'));
        let visibleCount = 0;
        items.forEach(function (item) {
            const matches = item.textContent.toLowerCase().includes(query);
            item.hidden = !matches;
            if (matches) visibleCount += 1;
        });
        if (emptyState) {
            emptyState.hidden = items.length > 0 && visibleCount > 0;
            emptyState.querySelector('strong').textContent = items.length > 0 ? 'No matches found' : 'No conversations yet';
            emptyState.querySelector('span:last-child').textContent = items.length > 0 ? 'Try a different search term.' : 'Messages with project partners will appear here.';
        }
    });

    setSidebarOpen(window.innerWidth > 720);
});


function getConvoIdFromPath() {
    const pathParts = convoPath.split('/');
    return pathParts[pathParts.length - 1];
}

function getConvoIdFromUrl() {
    const urlParams = new URLSearchParams(window.location.search);
    return urlParams.get('convo_id');
}   

function readLinesFromFile(filePath) { // info at top of file in brackets is the meta data showing the accounts involved/allowed, each message looks like this    ExampleUser9000: Hello (newline) #mm,dd,yyyy:,hr,mn,sc\n   , the chat can read this potentially as MD
    file.read


}