const steps = [
  {
    id: "1", kind: "model", label: "TYPED DECISION", title: "Find the test gaps",
    description: "A pull request starts the loop. Romeo looks at the change and works out which important behaviors still need a test.",
    output: "A short list of missing tests, ready for the next step."
  },
  {
    id: "2", kind: "ai", label: "LANGUAGE MODEL", title: "Write the test",
    description: "For each gap, a language model drafts a test and opens a test pull request. It suggests coverage; it doesn't decide whether the change is safe to merge.",
    output: "A proposed test pull request for an engineer to review."
  },
  {
    id: "3", kind: "runner", label: "RUNNERS & TESTS", title: "Run on main and releases",
    description: "The tests run on the main branch and release builds using CI runners and cloud instances. Compute and infrastructure are part of the cost too.",
    output: "Test results from real CI runs—not a model's guess."
  },
  {
    id: "4", kind: "model", label: "TYPED DECISION", title: "Classify each result",
    description: "Compare the result against an expected frame or value. Decide whether it passed, or whether a failure is real, flaky, or caused by infrastructure.",
    output: "A classified result. A real failure can move on to a first fix."
  },
  {
    id: "5", kind: "model", label: "TYPED DECISION", title: "Route the incoming bug",
    description: "A Jira ticket or Slack report comes in. Romeo identifies the affected area and likely owner, and checks whether it is a duplicate of a known issue.",
    output: "A bug with useful ownership and context."
  },
  {
    id: "6", kind: "ai", label: "LANGUAGE MODEL", title: "Try a first fix",
    description: "A language model proposes an initial fix for the routed bug or a real test failure. Hard cases can be escalated to a more capable model.",
    output: "A proposed fix pull request—not an automatic merge."
  },
  {
    id: "7", kind: "model", label: "HUMAN IN THE LOOP", title: "Check with a human",
    description: "An engineer reviews the proposed work before anything is posted or pushed. This check also covers the test pull request from step 2 and the fix pull request from step 6.",
    output: "A person stays in control of what gets shared or changed."
  },
  {
    id: "8", kind: "ai", label: "LANGUAGE MODEL", title: "Summarize for the engineer",
    description: "Prepare a short handoff: what failed, what was measured, and what fix is proposed, with a link to its pull request.",
    output: "A concise, reviewable summary with the important evidence."
  },
  {
    id: "9", kind: "runner", label: "RUNNERS & TESTS", title: "Make repeat checks deterministic",
    description: "When the same check is needed often, turn it into a regular pytest, ctest, or golden-frame comparison. The next run can be deterministic, with no model call.",
    output: "A reusable test that makes future checks faster and more predictable."
  }
];

const cardButtons = [...document.querySelectorAll("button.flow-card")];
const stepCount = document.querySelector("#step-count");
const progressFill = document.querySelector("#progress-fill");
const detailChip = document.querySelector("#detail-chip");
const detailStep = document.querySelector("#detail-step");
const detailTitle = document.querySelector("#detail-title");
const detailDescription = document.querySelector("#detail-description");
const detailOutput = document.querySelector("#detail-output");
const previousButton = document.querySelector("#prev-step");
const nextButton = document.querySelector("#next-step");
let currentIndex = 0;

function renderStep(index) {
  currentIndex = Math.max(0, Math.min(index, steps.length - 1));
  const step = steps[currentIndex];
  const number = String(currentIndex + 1).padStart(2, "0");
  const chipClass = step.kind === "ai" ? "ai-chip" : step.kind === "runner" ? "runner-chip" : "model-chip";

  stepCount.textContent = `${number} / ${String(steps.length).padStart(2, "0")}`;
  progressFill.style.width = `${((currentIndex + 1) / steps.length) * 100}%`;
  detailChip.className = `detail-chip ${chipClass}`;
  detailChip.textContent = step.label;
  detailStep.textContent = `STEP ${number}`;
  detailTitle.textContent = step.title;
  detailDescription.textContent = step.description;
  detailOutput.textContent = step.output;
  previousButton.disabled = currentIndex === 0;
  nextButton.disabled = currentIndex === steps.length - 1;

  cardButtons.forEach((button) => {
    const isActive = button.dataset.step === step.id;
    button.classList.toggle("is-active", isActive);
    button.setAttribute("aria-pressed", String(isActive));
  });
}

cardButtons.forEach((button) => {
  button.addEventListener("click", () => {
    const index = steps.findIndex((step) => step.id === button.dataset.step);
    if (index !== -1) renderStep(index);
  });
});

previousButton.addEventListener("click", () => renderStep(currentIndex - 1));
nextButton.addEventListener("click", () => renderStep(currentIndex + 1));

document.addEventListener("keydown", (event) => {
  if (event.altKey || event.ctrlKey || event.metaKey || event.target instanceof HTMLButtonElement) return;
  if (event.key === "ArrowLeft") renderStep(currentIndex - 1);
  if (event.key === "ArrowRight") renderStep(currentIndex + 1);
});

renderStep(0);
