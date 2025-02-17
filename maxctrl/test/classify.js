const { doCommand } = require("../test_utils.js");

describe("Classify Commands", function () {
  it("classifies query", function () {
    return doCommand("--tsv classify SELECT\t1").should.eventually.match(/Parser::Result::PARSED/);
  });

  it("classifies query with function", function () {
    // eslint-disable-next-line quotes
    return doCommand('--tsv classify SELECT\tspecial_function("hello",5)').should.eventually.match(
      /special_function/
    );
  });

  it("classifies invalid query", function () {
    return doCommand("--tsv classify This-should-fail").should.eventually.match(/Parser::Result::INVALID/);
  });

  it("rejects no query", function () {
    return doCommand("classify").should.be.rejected;
  });
});
