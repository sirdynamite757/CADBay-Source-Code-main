# CADBay - Web Interface

A creative design platform where ideas come to life.

## New Features

### Dark Mode
The website now includes a dark mode toggle button that:
- Switches between light and dark color schemes
- Saves user preference to browser localStorage
- Uses smooth color transitions for better UX

The toggle button is located in the top-right corner of the page.

### Toggle Button Images
Placeholder image files have been created in the `assets/` directory:
- `toggle-light.webp` - Light mode button (modern WebP format)
- `toggle-light.png` - Light mode button fallback (PNG format)
- `toggle-dark.webp` - Dark mode button (modern WebP format)
- `toggle-dark.png` - Dark mode button fallback (PNG format)

**Note:** Replace these placeholder files with your actual button images. The CSS will use WebP images where supported, with JPG as fallback.

## Running the Website

### Prerequisites
- GCC compiler (for compiling the C webserver)
- Linux/Unix environment

### Building the Webserver
```bash
cd /path/to/CADBay-Source-Code-main
make build
```

### Running the Webserver
```bash
make run
```

The server will start on port 3000 and display:
```
CADBay Server Started
Server listening on http://<YOUR_IP>:3000
Press Ctrl+C to stop the server
```

### Accessing the Site
Open your browser and navigate to:
```
http://<YOUR_IP>:3000
```

Where `<YOUR_IP>` is your current machine's IP address (shown when the server starts).

### Cleaning Up
```bash
make clean
```

## File Structure
```
├── Body/
│   ├── ColorDefault.css    - Main stylesheet with dark mode support
│   ├── Main.html          - Main page with dark mode toggle
│   ├── Header.html        - Header component
│   ├── Sidebar.html       - Sidebar component
│   ├── Footer.html        - Footer component
│   └── Start.html         - Start/welcome page
├── assets/                - Image assets
│   ├── toggle-light.webp  - Light mode toggle button (WebP)
│   ├── toggle-light.jpg   - Light mode toggle button (JPG fallback)
│   ├── toggle-dark.webp   - Dark mode toggle button (WebP)
│   └── toggle-dark.jpg    - Dark mode toggle button (JPG fallback)
├── RFPs/                  - RFP templates and tools
├── webserver.c            - C webserver implementation
├── webserver              - Compiled webserver executable
└── Makefile              - Build system
```

## Technical Details

### C Webserver Features
- HTTP/1.1 server listening on port 3000
- Serves static files (HTML, CSS, JS, images, fonts)
- Automatic MIME type detection
- Supports concurrent connections using fork()
- Graceful shutdown with Ctrl+C

### CSS Variables for Dark Mode
The stylesheet uses CSS custom properties (variables) for easy theme switching:
```css
:root {
    --primary-bg: #74573f;
    --primary-text: #ff0000;
    /* ... */
}

body.dark-mode {
    --primary-bg: #1a1a1a;
    --primary-text: #ff6b6b;
    /* ... */
}
```

### Dark Mode Toggle Script
Located in Main.html:
- Toggles `dark-mode` class on the body element
- Persists preference using localStorage
- Auto-loads saved preference on page reload

## Browser Support
- Modern browsers (Chrome, Firefox, Safari, Edge)
- Requires JavaScript enabled for dark mode toggle
- Falls back to light mode without localStorage support
