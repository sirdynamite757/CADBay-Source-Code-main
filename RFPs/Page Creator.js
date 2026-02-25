function* generateHtml() {
    yield `<title>Name of Project</title>`;
    yield `<p>First paragraph</p>`;
    yield `<p>Second paragraph</p>`;
}

const htmlGenerator = generateHtml();
document.title.innerHTML = "Name of Project";
document.body.innerHTML += htmlGenerator.next().value; // Adds first paragraph
document.body.innerHTML += htmlGenerator.next().value; // Adds second paragraph
