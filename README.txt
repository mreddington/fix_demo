This is the demo for your review.

To run the program, compile, and execute along the lines of:

  powershell> Get-Content Fix.Sample.txt | .\x64\Debug\fix_demo.exe

Standard error redirects to standard output by default, which is fine by me for this - you could obviously
redirect standard error to a file or pipe. My output looks like:

8=FIX.4.2|9=0314|35=AB|34=000000011|1=SPDRT1|11=10109A9578A82AAA|100=1|21=1|38=10|40=2|44=-3.98|54=1|55=BA|59=0|167=MLEG|207=XASE|60=20181127-16:46:07.996|204=8|555=2|600=BA|608=OP|609=OPT|611=20181109|612=360|654=EA628EA5B0F8186|623=1|624=2|564=O|600=BA|608=OP|609=OPT|611=20181109|612=362.5|654=EA628EA5B0F8187|623=1|624=2|564=O|10=195|
8=FIX.4.2|9=0308|35=AB|34=000000000|1=H20466|11=AFCDAF7E0FC8141|100=N|21=1|38=50|40=2|44=-11.7|54=1|55=NVDA|59=0|167=MLEG|60=20181127-16:46:07.996|204=0|555=3|600=NVDA|608=OP|609=OPT|611=20200117|612=215|654=EA61EEBE5226038|623=2|624=1|564=O|600=NVDA|608=OP|609=OPT|611=20210115|612=190|654=EA61EEBE5226039|623=1|624=2|564=O|10=078|
8=FIX.4.2|9=0151|35=D|34=000000014|1=SPDRT1|11=DB95A6B1070A29C|9303=Y|38=100|40=2|44=266.04|54=1|55=SPY|59=0|60=20181127-16:46:07.996|100=XNYS|9416=A|9303=N|528=A|386=1|336=2|10=028|
8=FIX.4.2|9=0151|35=D|34=000000000|1=SPDRT1|11=DB95A6B1070A29C|38=100|40=2|44=256.04|54=1|55=SPY|55=SPY|59=0|60=20181127-16:46:07.996|100=XNYS|9416=A|9303=N|528=A|386=1|336=2|10=027|
High/Low Report:
Account     High    Low
    ----------------------------
396CCS101   68.2    0.67
SPDRT1      266.04  256.04

Per the ask: "In your language of choice, please write a sample program that reads these FIX messages"

* I chose C++ as that is my greatest strength.
* I've implemented it in terms of C++20 and Microsoft Visual Studio 2022, as that is what I have on hand.
* That said, I'm mostly versed in C++11.
* The source code itself is sticking with pure C++ and not targeting platform specifics.

This is trading software, so speed still counts for something. Therefore,

* I dedicated to making a single pass.
* I'm aiming to minimize unnecessary reading and copying.

Per the ask: "Lists error notifications for messages that have the same field twice"

I deduced this means to write the offending messages to standard error.

Let's draw some ASCII diagrams!

                             ,-> stringstream -----------------------------------------|
                  stringbuf -\                                                         V        ,-> map
                              |-> buf tap -> stream tap -> tag/value_processor -> sink/visitor <
standard input -> streambuf -/         |                                                        `-> standard error
                             `-> cin   |
                                       L> ctype

The sample file is redirected into standard input. From there, it passes through the streambuf associated with
std::cin. To stay single pass and avoid backtracking, I needed a method of copying the message contents without
having to reposition the stream. I also didn't want logging logic cluttering the critical path, parsing code is
complex enough. Therefore I have a stream buffer similar to a tee - it reads from a source buffer, writes to a
destination buffer, and forwards the data. A tapped stream interfaces is just convenient.

A ctype was defined so separators and delimiters are treated as whitespace. Stream extractors delimit based on
whitespace, that's hard coded. What's not hard coded is what a whitespace character is. It saves me from ugly code
that has to manually purge these things out of the way.

Field IDs are extracted into a user defined type that represents a tag/value. The tag/value references the stream
because extracting the value is lazy, since we only care about account names and prices; I've only implemented
support for strings and currency. This was to avoid implementing a naive tuple and always extracting a value even
when we won't care about almost any of them.

The tag/value processor generates one production per field, which goes to the sink. The processor is a stateful
accumulator of message processing and value extraction, and implements the following logic:

  duplicate field -> produce error, reset
  account field ->
  price field ->
  message type -> cached locally
  all other fields -> ignored
  if end of message -> if complete information -> produce data, reset
                    -> produce purge, reset
  else -> produce no-op

The processor has a reference to the stream to advance the position.

The sink looks at the production and acts accordingly.

  error -> side stream to standard error
  purge -> clear the side stream
  data -> to map, clear the side stream
  no-op -> hurry up and wait

A standard transform drives the process, iterating over the stream of fields. The report is generated.

Note: My first pass included separating things out into their own files and I even had some GoogleTests, but I
didn't like how big the program was getting. All told, it's only 251 LOC and I don't think it's in scope to
boil the ocean trying to write a production capable FIX processor in an evening. There is definite room for
improvement I'm sure we'll have fun talking about.