`include "common.vh"

// =========================================================
// MODULE 1: DÙNG CHO WRAM, BRAM, CRAM (Có chức năng Hold)
// =========================================================
module Dual_Port_RAM
#(
  parameter AWIDTH = 13, 
  parameter DWIDTH = 32 
)
(
  input clka, input ena, input wea, input [AWIDTH-1:0] addra, input [DWIDTH-1:0] dina, output reg [DWIDTH-1:0] douta,
  input clkb, input enb, input web, input [AWIDTH-1:0] addrb, input [DWIDTH-1:0] dinb, output reg [DWIDTH-1:0] doutb 
);

	(* ram_style = "block" *) reg [DWIDTH-1:0] mem [2**AWIDTH-1:0];
	
	// Khởi tạo RAM = 0 để diệt lỗi X ban đầu
	integer i;
	initial begin
		for (i = 0; i < 2**AWIDTH; i = i + 1) mem[i] = 0;
		douta = 0; doutb = 0;
	end

	always @(posedge clka) begin
		if (ena) begin
			if (wea) mem[addra] <= dina;
			douta <= mem[addra]; // Giữ nguyên giá trị cũ nếu ena = 0
		end
	end
	
	always @(posedge clkb) begin
		if (enb) begin
			if (web) mem[addrb] <= dinb;
			doutb <= mem[addrb]; // Giữ nguyên giá trị cũ nếu enb = 0
		end
	end
endmodule

// =========================================================
// MODULE 2: DÙNG CHO LDM (Có chức năng xuất 0 để dùng cho OR-Tree)
// =========================================================
module Dual_Port_RAM_2
#(
  parameter AWIDTH = 10, 
  parameter DWIDTH = 32  
)
(
  input clka, input ena, input wea, input [AWIDTH-1:0] addra, input [DWIDTH-1:0] dina, output [DWIDTH-1:0] douta,
  input clkb, input enb, input web, input [AWIDTH-1:0] addrb, input [DWIDTH-1:0] dinb, output [DWIDTH-1:0] doutb 
);

	(* ram_style = "block" *) reg [DWIDTH-1:0] mem [2**AWIDTH-1:0];
	reg [DWIDTH-1:0] rdata_a = 0;
	reg [DWIDTH-1:0] rdata_b = 0;
	
	// Thêm thanh ghi delay Enable để khớp với độ trễ 1 clock của BRAM
	reg ena_reg = 0;
	reg enb_reg = 0;

	integer i;
	initial begin
		for (i = 0; i < 2**AWIDTH; i = i + 1) mem[i] = 0;
	end

	always @(posedge clka) begin
		ena_reg <= ena; // Ghi nhớ trạng thái Enable
		if (ena) begin
			if (wea) mem[addra] <= dina;
			rdata_a <= mem[addra];
		end
	end
	
	always @(posedge clkb) begin
		enb_reg <= enb; // Ghi nhớ trạng thái Enable
		if (enb) begin
			if (web) mem[addrb] <= dinb;
			rdata_b <= mem[addrb];
		end
	end

	// Chỉ nhả dữ liệu ra nếu nhịp clock trước đó có kích hoạt Enable
	assign douta = ena_reg ? rdata_a : 0;
	assign doutb = enb_reg ? rdata_b : 0;

endmodule