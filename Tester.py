# Also, use the absolute path of the file:
import webbrowser
import os
import http.server
import socketserver
#import Body
#import Header
#import Footer
#import Main
#import Sidebar
#import Navbar
#import Styles
#import Scripts
#import "/home/sirdynamite757/Desktop/Webpage"        Superseeded by the following code:
import sys

# Specify the folder path
#folder_path = '/home/sirdynamite757/Desktop/Webpage/Body'  not applicable in vs code
folder_path = 'Body' # Assuming 'Body' folder is in the current working directory

# Add the folder to the system path
sys.path.append(folder_path)

# Import all modules
for filename in os.listdir(folder_path):
    if filename.endswith('.py') and filename != '__init__.py':
        module_name = filename[:-3]  # Remove .py extension
        __import__(module_name)



from flask import Flask
app = Flask(__name__) 

'''import flask_cors
from fastapi import FastAPI

app = FastAPI()

@app.get("/")
def read_root():
    return {"Hello": "World"}

# To run the app, use: uvicorn filename:app --reload   no idea what happened here
import ipaddress

# For IPv4
ipv4 = ipaddress.ip_address('192.0.2.1')
print(ipv4)  # Output: IPv4Address('192.0.2.1')

# For IPv6
ipv6 = ipaddress.ip_address('2001:db8::1')
print(ipv6)  # Output: IPv6Address('2001:db8::1')'''



'''PORT = 1000  # Specify your desired port
IP_ADDRESS = '192.168.4.185'  # Replace with your specific IP address        this will probly be important later

Handler = http.server.SimpleHTTPRequestHandler
with socketserver.TCPServer((IP_ADDRESS, PORT), Handler) as httpd:
    print(f"Serving at {IP_ADDRESS}:{PORT}")
    httpd.serve_forever()

'''
# Project structure:
# my_project/
#     static/
#         ColorDefault.css
#         script.js
#         templates/            this is a whole pile of insanity
#         Main.html
#         Header.html
#         Footer.html
#         Sidebar.html
#         Navbar.html
#         StartPage.html
#     app.py

##my_project
'''static:
         ColorDefault.css
         script.js
     templates
         Main.html
         Header.html
         Footer.html
         Sidebar.html
         Navbar.html
         StartPage.html
     app.py '''

if __name__ == '__main__':
    app.run(debug=True) 